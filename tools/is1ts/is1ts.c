/*
 * is1ts - encode the IntelliStar's raw output for recording or streaming.
 *
 * The sibling of is1view. It reads the same thing - the Thunderstorm
 * model's raw 720x480 BGRA frames with their programme audio, from
 * `-device thunderstorm,output=/path/to/fifo` or TSC_OUTPUT= - but instead
 * of putting them in a window it hands them to ffmpeg. Files and TS network
 * outputs use MPEG-TS; RTMP uses FLV.
 *
 *   cc -O2 -pthread -o is1ts is1ts.c -lm
 *
 *   is1ts out.ts                                   # record to a file
 *   is1ts - | ffplay -                             # stdout
 *   is1ts 'udp://239.1.1.1:1234?pkt_size=1316'     # multicast
 *   is1ts 'srt://0.0.0.0:9000?mode=listener'       # SRT listener
 *   is1ts out.ts -c:v mpeg2video -b:v 8M -c:a mp2 -b:a 256k
 *                                                  # period-correct encode
 *
 * Options (before the output):
 *   -i PATH  the stream to read (default /tmp/is1-output)
 *   -k       encode the key plate (alpha) instead of the picture
 *   -K       both: picture as the first video stream, key as the second,
 *            so a downstream keyer gets fill and key in one TS
 *   -a DB    audio gain in dB, e.g. -a -6 or -a 3 (default 0). Applied
 *            here, before encoding, so it holds whatever encoder args you
 *            pass. Samples that would exceed full scale are clipped and
 *            counted; the count is reported when is1ts exits.
 *   -r       when the writer goes away, wait for the bench to come back
 *            and carry on in the same TS instead of finishing it
 *
 * Anything after the output replaces the default encoder arguments
 * (libx264 + AAC, tuned for live). ffmpeg must be on PATH.
 *
 * Ctrl-C finishes cleanly: ffmpeg gets the same SIGINT and closes the TS.
 */
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define W 720
#define H 480
#define FRAME_BYTES (W * H * 4)

/* Same wire format as is1view: 32-byte header, picture, then audio. */
#define HDR_BYTES 32
#define MAGIC 0x32535449u   /* 'ITS2' */
#define MAX_PAIRS 2048      /* the TSH_AUDIO plane holds no more */

/* 24-bit samples in 32-bit dwords; shift up to use the full S32 range. */
#define SAMPLE_SHIFT 8

/* The card paces at NTSC rate. The raw pipes carry no timestamps, so this
 * is the clock ffmpeg stamps the video with. */
#define FPS_NUM 30000
#define FPS_DEN 1001

static volatile sig_atomic_t stopping;

static void on_signal(int sig)
{
	(void)sig;
	stopping = 1;
}

static unsigned int rd32(const unsigned char *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

/*
 * Blocking read of exactly `want` bytes: 1 on success, 0 on EOF, error or
 * Ctrl-C. is1view had to juggle non-blocking reads because it also had a
 * window to service; nothing here has anything better to do than wait.
 */
static int read_exact(int fd, unsigned char *buf, size_t want)
{
	size_t got = 0;

	while (got < want) {
		ssize_t n = read(fd, buf + got, want - got);

		if (n > 0) {
			got += (size_t)n;
			continue;
		}
		if (n == 0) {
			return 0;
		}
		if (errno == EINTR && !stopping) {
			continue;
		}
		if (errno != EINTR) {
			perror("is1ts: read");
		}
		return 0;
	}
	return 1;
}

static int write_all(int fd, const void *p, size_t len)
{
	const unsigned char *b = p;

	while (len) {
		ssize_t n = write(fd, b, len);

		if (n > 0) {
			b += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		return -1;          /* EPIPE: ffmpeg has gone */
	}
	return 0;
}

/*
 * One writer thread per pipe, each behind its own queue.
 *
 * Writing all the pipes from one thread deadlocks: while ffmpeg is opening
 * and probing one input it is not reading the others, so a blocked write on
 * the pipe it is ignoring stops us feeding the one it is waiting for. With a
 * thread per pipe, each pipe drains at whatever pace ffmpeg takes it, and
 * the queues absorb the difference. They are bounded (~1.5 s of picture) so
 * a stalled ffmpeg pushes back instead of eating memory.
 */
#define QUEUE_LIMIT (64u << 20)

struct chunk {
	struct chunk *next;
	size_t len;
	unsigned char data[];
};

struct outq {
	int fd;
	pthread_t th;
	pthread_mutex_t m;
	pthread_cond_t cv;
	struct chunk *head, *tail;
	size_t bytes;
	int closed, failed;
};

static void *outq_run(void *arg)
{
	struct outq *q = arg;

	for (;;) {
		struct chunk *c;

		pthread_mutex_lock(&q->m);
		while (!q->head && !q->closed) {
			pthread_cond_wait(&q->cv, &q->m);
		}
		c = q->head;
		if (!c) {
			pthread_mutex_unlock(&q->m);
			break;
		}
		q->head = c->next;
		if (!q->head) {
			q->tail = NULL;
		}
		pthread_mutex_unlock(&q->m);

		if (write_all(q->fd, c->data, c->len)) {
			pthread_mutex_lock(&q->m);
			q->failed = 1;
			pthread_cond_broadcast(&q->cv);
			pthread_mutex_unlock(&q->m);
			free(c);
			break;
		}
		pthread_mutex_lock(&q->m);
		q->bytes -= c->len;
		pthread_cond_broadcast(&q->cv);
		pthread_mutex_unlock(&q->m);
		free(c);
	}
	close(q->fd);           /* EOF is ffmpeg's cue to finish this input */
	return NULL;
}

static int outq_start(struct outq *q, int fd)
{
	memset(q, 0, sizeof *q);
	q->fd = fd;
	pthread_mutex_init(&q->m, NULL);
	pthread_cond_init(&q->cv, NULL);
	return pthread_create(&q->th, NULL, outq_run, q);
}

static int outq_put(struct outq *q, const void *p, size_t len)
{
	struct chunk *c = malloc(sizeof *c + len);

	if (!c) {
		return -1;
	}
	c->next = NULL;
	c->len = len;
	memcpy(c->data, p, len);

	pthread_mutex_lock(&q->m);
	while (q->bytes > QUEUE_LIMIT && !q->failed && !stopping) {
		struct timespec deadline;

		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_nsec += 100000000;
		if (deadline.tv_nsec >= 1000000000) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&q->cv, &q->m, &deadline);
	}
	if (q->failed || stopping) {
		pthread_mutex_unlock(&q->m);
		free(c);
		return -1;
	}
	if (q->tail) {
		q->tail->next = c;
	} else {
		q->head = c;
	}
	q->tail = c;
	q->bytes += len;
	pthread_cond_broadcast(&q->cv);
	pthread_mutex_unlock(&q->m);
	return 0;
}

/* Signal every input's EOF before waiting for any writer. FFmpeg may be
 * probing another input while the first writer is blocked on its pipe. */
static void outq_close(struct outq *q)
{
	pthread_mutex_lock(&q->m);
	q->closed = 1;
	pthread_cond_broadcast(&q->cv);
	pthread_mutex_unlock(&q->m);
}

static void outq_join(struct outq *q)
{
	pthread_join(q->th, NULL);
}

struct frame {
	unsigned int pairs;
	unsigned int rate;
};

static unsigned char abuf[MAX_PAIRS * 2 * 4];

/*
 * 1 = a frame, 0 = stream ended, -1 = false header, keep scanning.
 *
 * Resynchronising on the magic is the normal startup path, not an error:
 * attaching to a live FIFO lands mid-frame. The header is sanity-checked
 * before the body is read so that a stray 'ITS2' in picture data costs a
 * few bytes of scanning, not a whole frame.
 */
static int read_frame(int fd, unsigned char *buf, struct frame *f)
{
	unsigned char hdr[HDR_BYTES];

	if (!read_exact(fd, hdr, HDR_BYTES)) {
		return 0;
	}
	while (rd32(hdr) != MAGIC) {
		memmove(hdr, hdr + 1, HDR_BYTES - 1);
		if (!read_exact(fd, hdr + HDR_BYTES - 1, 1)) {
			return 0;
		}
	}
	if (rd32(hdr + 4) != W || rd32(hdr + 8) != H) {
		return -1;
	}
	f->pairs = rd32(hdr + 16);
	f->rate = rd32(hdr + 20);
	if (f->pairs > MAX_PAIRS ||
	    (f->rate && (f->rate < 8000 || f->rate > 192000))) {
		return -1;
	}
	if (!read_exact(fd, buf, FRAME_BYTES)) {
		return 0;
	}
	if (f->pairs && !read_exact(fd, abuf, f->pairs * 2 * 4)) {
		return 0;
	}
	return 1;
}

/*
 * Start ffmpeg with the read ends of `n` pipes as its fds 3, 4 (and 5), so
 * it can take video and audio as separate raw inputs ("pipe:3", "pipe:4").
 * macOS has no pipe2(), hence the fcntl dance; the read ends are first
 * parked above 10 so dup2 onto 3..5 cannot clobber one another.
 */
static pid_t spawn_ffmpeg(char **args, int *wfd, int n)
{
	int p[3][2];
	int i;
	pid_t pid;

	for (i = 0; i < n; i++) {
		if (pipe(p[i]) != 0) {
			perror("is1ts: pipe");
			return -1;
		}
		fcntl(p[i][0], F_SETFD, FD_CLOEXEC);
		fcntl(p[i][1], F_SETFD, FD_CLOEXEC);
	}
	pid = fork();
	if (pid < 0) {
		perror("is1ts: fork");
		return -1;
	}
	if (pid == 0) {
		int hi[3];

		for (i = 0; i < n; i++) {
			hi[i] = fcntl(p[i][0], F_DUPFD_CLOEXEC, 10);
		}
		for (i = 0; i < n; i++) {
			dup2(hi[i], 3 + i);     /* dup2 clears close-on-exec */
		}
		execvp(args[0], args);
		fprintf(stderr, "is1ts: cannot run ffmpeg: %s\n", strerror(errno));
		_exit(127);
	}
	for (i = 0; i < n; i++) {
		close(p[i][0]);
		wfd[i] = p[i][1];
	}
	return pid;
}

static uint64_t frames_out, audio_out;

/*
 * Gain. 1.0 means untouched, and takes the plain integer path so the
 * default output is bit-exact with what the card carried.
 */
static double gain = 1.0;
static uint64_t clipped;

static int32_t apply_gain(int32_t v)
{
	double x;

	if (gain == 1.0) {
		return v;
	}
	x = (double)v * gain;
	if (x > INT32_MAX) {
		clipped++;
		return INT32_MAX;
	}
	if (x < INT32_MIN) {
		clipped++;
		return INT32_MIN;
	}
	return (int32_t)lrint(x);
}

/*
 * Send one frame: picture, then its audio, then (for -K) its key.
 *
 * A/V sync falls out of counting. Video is stamped at exactly 29.97 by
 * frame number and audio at 48 kHz by sample number, and the card's audio
 * already carries the NTSC cadence, so as long as every frame's audio goes
 * in with it the two cannot drift. A frame that arrives with no audio gets
 * silence to fill the gap, so the next real audio still lines up.
 *
 */
static int emit(struct outq *wq, int mode, const unsigned char *buf,
                unsigned char *key, const struct frame *f, unsigned int rate)
{
	if (mode != 1 && outq_put(&wq[0], buf, FRAME_BYTES)) {
		return -1;
	}
	frames_out++;

	if (f->pairs) {
		static int32_t s[MAX_PAIRS * 2];
		unsigned int i;

		for (i = 0; i < f->pairs * 2; i++) {
			s[i] = apply_gain((int32_t)(rd32(abuf + i * 4) << SAMPLE_SHIFT));
		}
		if (outq_put(&wq[1], s, f->pairs * 2 * sizeof(int32_t))) {
			return -1;
		}
		audio_out += f->pairs;
	} else {
		static const int32_t zero[MAX_PAIRS * 2];
		uint64_t due = frames_out * rate * FPS_DEN / FPS_NUM;

		while (audio_out < due) {
			uint64_t k = due - audio_out;

			if (k > MAX_PAIRS) {
				k = MAX_PAIRS;
			}
			if (outq_put(&wq[1], zero, (size_t)k * 2 * sizeof(int32_t))) {
				return -1;
			}
			audio_out += k;
		}
	}

	if (mode != 0) {
		/* The key plate: what the downstream keyer cuts the fill with. */
		int i;

		for (i = 0; i < W * H; i++) {
			key[i] = buf[i * 4 + 3];
		}
		if (outq_put(&wq[mode == 1 ? 0 : 2], key, W * H)) {
			return -1;
		}
	}
	return 0;
}

static int open_input(const char *path)
{
	/* Blocks until the bench opens its end, which is what we want. */
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 && !stopping) {
		fprintf(stderr, "is1ts: cannot open %s: %s\n"
		        "       (is the bench running with TSC_OUTPUT set?)\n",
		        path, strerror(errno));
	}
	return fd;
}

static void usage(void)
{
	fprintf(stderr,
	        "usage: is1ts [-i fifo] [-k | -K] [-a gain_dB] [-r] OUTPUT [ffmpeg encoder args...]\n"
	        "  OUTPUT: out.ts, -, udp://..., srt://..., or rtmp://...\n");
	exit(2);
}

static int is_rtmp_url(const char *out)
{
	const char *scheme_end = strstr(out, "://");
	size_t n;

	if (!scheme_end) return 0;
	n = (size_t)(scheme_end - out);
	return (n == 4 && !strncasecmp(out, "rtmp", n)) ||
	       (n == 5 && (!strncasecmp(out, "rtmps", n) ||
	                   !strncasecmp(out, "rtmpt", n) ||
	                   !strncasecmp(out, "rtmpe", n))) ||
	       (n == 6 && (!strncasecmp(out, "rtmpts", n) ||
	                   !strncasecmp(out, "rtmpte", n)));
}

static int incompatible_rtmp_codec(const char *arg)
{
	return !strcasecmp(arg, "mpeg2video") || !strcasecmp(arg, "mp2");
}

int main(int argc, char **argv)
{
	const char *in = "/tmp/is1-output";
	const char *out;
	int mode = 0;           /* 0 picture, 1 key, 2 picture + key */
	int reconnect = 0;
	int rtmp;
	int fd, c, r, status, nin, i;
	int wfd[3];
	struct outq wq[3];
	sigset_t block, old;
	unsigned char *buf, *key;
	unsigned int rate;
	struct frame f;
	struct sigaction sa;
	pid_t pid;
	char rate_s[16];
	char *args[96];
	int na = 0;

	static char *default_enc[] = {
		"-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
		"-pix_fmt", "yuv420p",
		"-b:v", "6M", "-maxrate", "6M", "-bufsize", "3M",
		"-g", "15",             /* a keyframe every half second, for joiners */
		"-c:a", "aac", "-b:a", "192k",
		NULL
	};

	while ((c = getopt(argc, argv, "+i:kKa:rh")) != -1) {
		switch (c) {
		case 'i': in = optarg; break;
		case 'k': mode = 1; break;
		case 'K': mode = 2; break;
		case 'a': {
			char *end;
			double db = strtod(optarg, &end);

			if (end == optarg || *end || db < -60 || db > 40) {
				fprintf(stderr, "is1ts: -a wants a gain in dB, -60 to 40\n");
				return 2;
			}
			gain = pow(10.0, db / 20.0);
			break;
		}
		case 'r': reconnect = 1; break;
		default: usage();
		}
	}
	if (optind >= argc) {
		usage();
	}
	out = argv[optind];
	rtmp = is_rtmp_url(out);
	if (rtmp && mode == 2) {
		fprintf(stderr, "is1ts: RTMP carries one video stream; -K needs MPEG-TS output\n");
		return 2;
	}
	if (rtmp) {
		for (i = optind + 1; i + 1 < argc; i++) {
			if ((!strcmp(argv[i], "-c:v") || !strcmp(argv[i], "-codec:v") ||
			     !strcmp(argv[i], "-vcodec") || !strcmp(argv[i], "-c:a") ||
			     !strcmp(argv[i], "-codec:a") || !strcmp(argv[i], "-acodec")) &&
			    incompatible_rtmp_codec(argv[i + 1])) {
				fprintf(stderr,
				        "is1ts: MPEG-2 video/MP2 audio cannot be sent to RTMP; "
				        "omit encoder options to use H.264/AAC\n");
				return 2;
			}
		}
	}

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;      /* no SA_RESTART: let reads see EINTR */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	buf = malloc(FRAME_BYTES);
	key = malloc(W * H);
	if (!buf || !key) {
		return 1;
	}

	fd = open_input(in);
	if (fd < 0) {
		return 1;
	}

	/* ffmpeg needs the audio rate up front, and only the header has it. */
	while ((r = read_frame(fd, buf, &f)) == -1) {
		;
	}
	if (r == 0) {
		fprintf(stderr, "is1ts: stream ended before the first frame\n");
		return 1;
	}
	rate = f.rate ? f.rate : 48000;
	snprintf(rate_s, sizeof rate_s, "%u", rate);

	args[na++] = "ffmpeg";
	args[na++] = "-hide_banner";
	args[na++] = "-loglevel";   args[na++] = "warning";
	args[na++] = "-nostdin";

	args[na++] = "-thread_queue_size"; args[na++] = "1024";
	args[na++] = "-probesize"; args[na++] = "32";
	args[na++] = "-analyzeduration"; args[na++] = "0";
	args[na++] = "-f";          args[na++] = "rawvideo";
	args[na++] = "-pix_fmt";    args[na++] = mode == 1 ? "gray" : "bgra";
	args[na++] = "-s";          args[na++] = "720x480";
	args[na++] = "-framerate";  args[na++] = "30000/1001";
	args[na++] = "-i";          args[na++] = "pipe:3";

	args[na++] = "-thread_queue_size"; args[na++] = "1024";
	args[na++] = "-probesize"; args[na++] = "32";
	args[na++] = "-analyzeduration"; args[na++] = "0";
	args[na++] = "-f";          args[na++] = "s32le";
	args[na++] = "-ar";         args[na++] = rate_s;
	args[na++] = "-ac";         args[na++] = "2";
	args[na++] = "-i";          args[na++] = "pipe:4";

	if (mode == 2) {
		args[na++] = "-thread_queue_size"; args[na++] = "1024";
		args[na++] = "-probesize"; args[na++] = "32";
		args[na++] = "-analyzeduration"; args[na++] = "0";
		args[na++] = "-f";          args[na++] = "rawvideo";
		args[na++] = "-pix_fmt";    args[na++] = "gray";
		args[na++] = "-s";          args[na++] = "720x480";
		args[na++] = "-framerate";  args[na++] = "30000/1001";
		args[na++] = "-i";          args[na++] = "pipe:5";
	}

	args[na++] = "-map"; args[na++] = "0:v";
	args[na++] = "-map"; args[na++] = "1:a";
	if (mode == 2) {
		args[na++] = "-map"; args[na++] = "2:v";
	}

	if (optind + 1 < argc) {
		for (i = optind + 1; i < argc && na < 80; i++) {
			args[na++] = argv[i];
		}
	} else {
		for (i = 0; default_enc[i]; i++) {
			args[na++] = default_enc[i];
		}
	}

	/* 720x480 is 4:3 on air; say so, or players show it stretched. */
	args[na++] = "-aspect";     args[na++] = "4:3";
	if (!rtmp) {
		args[na++] = "-metadata"; args[na++] = "service_provider=VirTSC";
		args[na++] = "-metadata"; args[na++] = "service_name=IntelliStar";
	} else {
		args[na++] = "-flvflags"; args[na++] = "no_duration_filesize";
	}
	/*
	 * Send live packets promptly. FFmpeg's TS muxer otherwise holds 0.7 s
	 * before a pipe reader (MistServer's ts-exec:, ffplay) sees them.
	 */
	args[na++] = "-muxdelay";   args[na++] = "0";
	args[na++] = "-muxpreload"; args[na++] = "0";
	args[na++] = "-flush_packets"; args[na++] = "1";
	args[na++] = "-f";          args[na++] = rtmp ? "flv" : "mpegts";
	args[na++] = (char *)out;
	args[na] = NULL;

	nin = mode == 2 ? 3 : 2;
	pid = spawn_ffmpeg(args, wfd, nin);
	if (pid < 0) {
		return 1;
	}
	/* Writer threads must not take our SIGINT/SIGTERM: the main thread's
	 * read() is what has to be interrupted. */
	sigemptyset(&block);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &block, &old);
	for (i = 0; i < nin; i++) {
		if (outq_start(&wq[i], wfd[i]) != 0) {
			fprintf(stderr, "is1ts: cannot start writer thread\n");
			return 1;
		}
	}
	pthread_sigmask(SIG_SETMASK, &old, NULL);

	fprintf(stderr, "is1ts: %s -> %s (%s, %u Hz, gain %+.1f dB)\n", in,
	        rtmp ? "RTMP destination" : out,
	        mode == 0 ? "picture" : mode == 1 ? "key" : "picture + key", rate,
	        20.0 * log10(gain));

	for (;;) {
		if (emit(wq, mode, buf, key, &f, rate)) {
			fprintf(stderr, "is1ts: ffmpeg stopped taking input\n");
			break;
		}
		while ((r = read_frame(fd, buf, &f)) == -1) {
			;
		}
		if (r == 1) {
			continue;
		}
		if (!reconnect || stopping) {
			break;
		}
		/* Bench went away: keep the TS open and wait for it. */
		close(fd);
		fprintf(stderr, "is1ts: writer gone, waiting for it to return\n");
		fd = open_input(in);
		if (fd < 0) {
			break;
		}
		while ((r = read_frame(fd, buf, &f)) == -1) {
			;
		}
		if (r != 1) {
			break;
		}
	}

	/* A service stop must not wait for a stalled ffmpeg input to drain. */
	if (stopping) {
		kill(pid, SIGTERM);
	}
	/* Drain and close the pipes: ffmpeg's cue to flush and finish the TS. */
	for (i = 0; i < nin; i++) {
		outq_close(&wq[i]);
	}
	for (i = 0; i < nin; i++) {
		outq_join(&wq[i]);
	}
	if (fd >= 0) {
		close(fd);
	}
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
		;
	}
	fprintf(stderr, "is1ts: %llu frames written\n",
	        (unsigned long long)frames_out);
	if (clipped) {
		fprintf(stderr, "is1ts: %llu audio samples clipped - lower the gain\n",
		        (unsigned long long)clipped);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
