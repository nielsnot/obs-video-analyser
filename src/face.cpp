#ifdef _WIN32
#define NOMINMAX // windows.h, pulled in by the OBS headers, defines min and max as macros otherwise
#endif
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include <cmath>
#include "facedetectcnn.h"
#include "face.h"

#define DETECT_WIDTH 320
#define MIN_CONFIDENCE 60
#define LOST_AFTER_NS 1000000000ULL

struct tracked_face
{
	face_box box;
	uint64_t seen_ns;
};

struct face_detector
{
	std::thread worker;
	std::mutex mutex;
	std::condition_variable cond;
	bool stop = false;
	bool pending = false;

	std::vector<uint8_t> image; // BGR, filled by submit
	int width = 0, height = 0;

	std::vector<tracked_face> faces; // guarded by mutex
	int next_id = 1;
};

static float iou(const face_box &a, const face_box &b)
{
	const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
	const float x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
	const float inter = std::max(0.0f, x1 - x0) * std::max(0.0f, y1 - y0);
	const float uni = a.w * a.h + b.w * b.h - inter;
	return uni > 0.0f ? inter / uni : 0.0f;
}

static void worker_loop(face_detector *fd)
{
	std::vector<uint8_t> image;
	std::vector<uint8_t> buffer(FACEDETECTION_RESULT_BUFFER_SIZE);
	os_set_thread_name("video-analyser-faces");

	for (;;) {
		int w, h;
		{
			std::unique_lock<std::mutex> lock(fd->mutex);
			fd->cond.wait(lock, [fd]() { return fd->stop || fd->pending; });
			if (fd->stop)
				return;
			image.swap(fd->image);
			w = fd->width;
			h = fd->height;
		}

		int *res = facedetect_cnn(buffer.data(), image.data(), w, h, w * 3);
		std::vector<face_box> found;
		for (int i = 0; res && i < *res && (int)found.size() < 32; i++) {
			const short *p = ((const short *)(res + 1)) + FACEDETECTION_RESULT_STRIDE_SHORTS * i;
			if (p[0] < MIN_CONFIDENCE)
				continue;
			face_box b = {};
			b.confidence = p[0] / 100.0f;
			b.x = (float)p[1] / w;
			b.y = (float)p[2] / h;
			b.w = (float)p[3] / w;
			b.h = (float)p[4] / h;
			for (int k = 0; k < 5; k++) {
				b.landmarks[k * 2] = (float)p[5 + k * 2] / w;
				b.landmarks[k * 2 + 1] = (float)p[5 + k * 2 + 1] / h;
			}
			found.push_back(b);
		}

		const uint64_t now = os_gettime_ns();
		std::lock_guard<std::mutex> lock(fd->mutex);
		// match to the faces already tracked; smooth their boxes so they do not jitter
		std::vector<bool> used(fd->faces.size(), false);
		for (face_box &b : found) {
			int best = -1;
			float best_iou = 0.3f;
			for (size_t t = 0; t < fd->faces.size(); t++) {
				const float v = iou(b, fd->faces[t].box);
				if (!used[t] && v > best_iou) {
					best_iou = v;
					best = (int)t;
				}
			}
			if (best >= 0) {
				used[best] = true;
				tracked_face &t = fd->faces[best];
				const float a = 0.5f;
				t.box.x += (b.x - t.box.x) * a;
				t.box.y += (b.y - t.box.y) * a;
				t.box.w += (b.w - t.box.w) * a;
				t.box.h += (b.h - t.box.h) * a;
				for (int k = 0; k < 10; k++)
					t.box.landmarks[k] += (b.landmarks[k] - t.box.landmarks[k]) * a;
				t.box.confidence = b.confidence;
				t.seen_ns = now;
			} else {
				tracked_face t;
				t.box = b;
				t.box.id = fd->next_id++;
				t.seen_ns = now;
				fd->faces.push_back(t);
				used.push_back(true);
			}
		}
		fd->faces.erase(
			std::remove_if(fd->faces.begin(), fd->faces.end(),
				       [now](const tracked_face &t) { return now - t.seen_ns > LOST_AFTER_NS; }),
			fd->faces.end());
		std::sort(fd->faces.begin(), fd->faces.end(), [](const tracked_face &a, const tracked_face &b) {
			return a.box.w * a.box.h > b.box.w * b.box.h;
		});
		fd->pending = false;
	}
}

extern "C" struct face_detector *face_detector_create(void)
{
	auto *fd = new face_detector;
	fd->worker = std::thread(worker_loop, fd);
	return fd;
}

extern "C" void face_detector_destroy(struct face_detector *fd)
{
	if (!fd)
		return;
	{
		std::lock_guard<std::mutex> lock(fd->mutex);
		fd->stop = true;
	}
	fd->cond.notify_all();
	if (fd->worker.joinable())
		fd->worker.join();
	delete fd;
}

extern "C" void face_detector_submit(struct face_detector *fd, const uint8_t *bgra, uint32_t width, uint32_t height,
				     uint32_t linesize)
{
	if (!fd || !bgra || !width || !height)
		return;
	{
		std::lock_guard<std::mutex> lock(fd->mutex);
		if (fd->pending)
			return; // still working on the last one
	}
	// scale down to at most DETECT_WIDTH, nearest neighbour, BGRA to BGR
	const uint32_t w = std::min<uint32_t>(width, DETECT_WIDTH);
	const uint32_t h = std::max<uint32_t>(1, (uint32_t)((uint64_t)height * w / width));
	std::vector<uint8_t> img((size_t)w * h * 3);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = bgra + (size_t)linesize * ((uint64_t)y * height / h);
		uint8_t *d = img.data() + (size_t)y * w * 3;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *s = row + ((uint64_t)x * width / w) * 4;
			d[x * 3 + 0] = s[0];
			d[x * 3 + 1] = s[1];
			d[x * 3 + 2] = s[2];
		}
	}
	{
		std::lock_guard<std::mutex> lock(fd->mutex);
		fd->image.swap(img);
		fd->width = (int)w;
		fd->height = (int)h;
		fd->pending = true;
	}
	fd->cond.notify_one();
}

extern "C" int face_detector_get(struct face_detector *fd, struct face_box *out, int max)
{
	if (!fd)
		return 0;
	std::lock_guard<std::mutex> lock(fd->mutex);
	int n = 0;
	for (const tracked_face &t : fd->faces) {
		if (n >= max)
			break;
		out[n++] = t.box;
	}
	return n;
}
