#include "vdecoder.h" //* video decode library in "libcedarc/include/"
#include "memoryAdapter.h"
/* create dumb buffer */
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define VIDEO_FILE_NAME "video.mjpeg"
#define OVERLAY_FILE_NAME "overlay.png"

#define DRM_FORMAT_MOD_VENDOR_ALLWINNER 0x09
#define fourcc_mod_code(vendor, val)                                           \
  ((((__u64)DRM_FORMAT_MOD_VENDOR_##vendor) << 56) |                           \
   ((val) & 0x00ffffffffffffffULL))
#define DRM_FORMAT_MOD_ALLWINNER_TILED fourcc_mod_code(ALLWINNER, 1)

static inline int DRM_IOCTL(int fd, unsigned long cmd, void *arg) {
  int ret = drmIoctl(fd, cmd, arg);
  return ret < 0 ? -errno : ret;
}

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

struct buffer_object {
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t handle;
  uint32_t size;
  uint8_t *vaddr;
  uint32_t fb_id;
};

struct property_arg {
  uint32_t obj_id;
  uint32_t obj_type;
  char name[DRM_PROP_NAME_LEN + 1];
  uint32_t prop_id;
  uint64_t value;
};

struct buffer_object buf;
struct buffer_object plane_buf[4];
drmModeConnector *conn;
drmModeRes *res;
drmModePlaneRes *plane_res;
int drm_fd;
uint32_t crtc_id;

uint8_t *load_image_BGRA8888(const char *filename, int *width, int *height) {
  int channels;
  uint8_t *data = stbi_load(filename, width, height, &channels, 4);
  if (!data) {
    printf("Failed to load image %s\n", filename);
    return NULL;
  }
  return data;
}

static void write_color(struct buffer_object *bo, unsigned int color) {
  unsigned int *pt;
  int i;

  pt = (unsigned int *)bo->vaddr;
  for (i = 0; i < (bo->size / 4); i++) {
    *pt = color;
    pt++;
  }
}
static void write_color_half(struct buffer_object *bo, unsigned int color1,
                             unsigned int color2) {
  unsigned int *pt;
  int i, size;

  pt = (unsigned int *)bo->vaddr;
  size = bo->size / 4 / 5;
  for (i = 0; i < (size); i++) {
    *pt = (139 << 16) | 255;
    pt++;
  }
  for (i = 0; i < (size); i++) {
    *pt = 255;
    pt++;
  }
  for (i = 0; i < (size); i++) {
    *pt = (127 << 8) | 255;
    pt++;
  }
  for (i = 0; i < (size); i++) {
    *pt = 255 << 8;
    pt++;
  }
  for (i = 0; i < (size); i++) {
    *pt = (255 << 16) | (255 << 8);
    pt++;
  }
}

void yuvmb32_2_rgb565(int width, int height, uint16_t *out, uint8_t *luma,
                      uint8_t *chroma) {
  int i = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int cy = y / 2;
      int Y =
          *((uint8_t *)(luma + (x / 32) * 1024 + (x % 32) + ((y % 32) * 32) +
                        ((y / 32) * (((width + 31) / 32) * 1024))));
      int Cb = *((uint8_t *)(chroma + (x / 32) * 1024 + ((x % 32) / 2 * 2) +
                             ((cy % 32) * 32) +
                             ((cy / 32) * (((width + 31) / 32) * 1024)))) -
               128;
      int Cr = *((uint8_t *)(chroma + (x / 32) * 1024 + ((x % 32) / 2 * 2 + 1) +
                             ((cy % 32) * 32) +
                             ((cy / 32) * (((width + 31) / 32) * 1024)))) -
               128;
      int R = Y + 359 * Cr / 256; // 1.402 * Cr;
      int G = Y - 88 * Cb / 256 -
              2925 * Cr / 4096;     // 0.344136 * Cb - 0.714136 * Cr;
      int B = Y + 3629 * Cb / 2048; // 1.772 * Cb;
      if (R > 255)
        R = 255;
      else if (R < 0)
        R = 0;
      if (G > 255)
        G = 255;
      else if (G < 0)
        G = 0;
      if (B > 255)
        B = 255.0;
      else if (B < 0)
        B = 0;
      out[i++] = ((R & 0b11111000) << 8) | ((G & 0b11111100) << 3) | (B >> 3);
    }
  }
}

void mb32dec(int width, int height, uint16_t *out, uint8_t *luma,
             uint8_t *chroma) {
  int x, y, cy;
  uint32_t *ptr = (uint32_t *)out;
  for (y = 0; y < height; y++) {
    cy = y / 2;
    for (x = 0; x < width; x += 32) {
      uint32_t *pl =
          ((uint32_t *)(luma + (x / 32) * 1024 + (x % 32) + ((y % 32) * 32) +
                        ((y / 32) * (((width + 31) / 32) * 1024))));
      uint32_t *pc = ((uint32_t *)(chroma + (x / 32) * 1024 +
                                   ((x % 32) / 2 * 2) + ((cy % 32) * 32) +
                                   ((cy / 32) * (((width + 31) / 32) * 1024))));
      for (int i = 0; i < 8; i++) {
        uint32_t Y = *pl++;
        uint32_t C = *pc++;
        *ptr++ = ((Y & 0x0000ff00) << 16) | ((C & 0x000000ff) << 16) |
                 ((Y & 0x000000ff) << 8) | ((C & 0x0000ff00) >> 8);
        *ptr++ = (Y & 0xff000000) | (C & 0x00ff0000) | ((Y & 0x00ff0000) >> 8) |
                 ((C & 0xff000000) >> 24);
      }
    }
  }
}

static int modeset_create_fb(int fd, struct buffer_object *bo, int planemode,
                             int bpp, uint64_t modifier) {
  struct drm_mode_create_dumb creq;
  struct drm_mode_destroy_dumb dreq;
  struct drm_mode_map_dumb mreq;
  uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
  uint64_t modifiers[4] = {0};
  int ret;

  memset(&creq, 0, sizeof(creq));
  creq.width = bo->width;
  creq.height = bo->height;
  creq.bpp = bpp;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
  if (ret < 0) {
    fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
    return -errno;
  }
  bo->pitch = creq.pitch;
  bo->size = creq.size;
  bo->handle = creq.handle;

  printf("pitch %d,size %d,handle %d,modifier %llx\n", bo->pitch, bo->size,
         bo->handle, modifier);

  offsets[0] = 0;
  handles[0] = bo->handle;
  pitches[0] = bo->pitch;
  modifiers[0] = modifier;

  if (modifier == DRM_FORMAT_MOD_ALLWINNER_TILED) {
    printf("drmModeAddFB2 with MOD\n", ret);
    ret = drmModeAddFB2WithModifiers(fd, bo->width, bo->height, planemode,
                                     handles, pitches, offsets, modifiers,
                                     &bo->fb_id, DRM_MODE_FB_MODIFIERS);
  } else {
    ret = drmModeAddFB2(fd, bo->width, bo->height, planemode, handles, pitches,
                        offsets, &bo->fb_id, 0);
  }
  if (ret) {
    printf("drmModeAddFB2 return err %d\n", ret);
    return 0;
  }

  /* prepare buffer for memory mapping */
  memset(&mreq, 0, sizeof(mreq));
  mreq.handle = bo->handle;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
  if (ret) {
    fprintf(stderr, "cannot map dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* perform actual memory mapping */
  bo->vaddr =
      mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
  if (bo->vaddr == MAP_FAILED) {
    fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n", errno);
    ret = -errno;
    goto err_fb;
  }

  /* clear the framebuffer to 0 */
  memset(bo->vaddr, 0, bo->size);

  return 0;

err_fb:
  drmModeRmFB(fd, bo->fb_id);
err_destroy:
  memset(&dreq, 0, sizeof(dreq));
  dreq.handle = bo->handle;
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
  return ret;
}

static int modeset_create_yuvfb(int fd, struct buffer_object *bo) {
  struct drm_mode_create_dumb create = {};
  struct drm_mode_map_dumb map = {};
  uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
  int ret;

  create.width = bo->width;
  create.height = bo->height;
  create.bpp = 8;
  drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);

  bo->pitch = create.pitch;
  bo->size = create.size;
  bo->handle = create.handle;

  map.handle = create.handle;
  drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);

  bo->vaddr =
      mmap(0, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);

  offsets[0] = 0;
  handles[0] = bo->handle;
  pitches[0] = bo->pitch;

  ret = drmModeAddFB2(fd, bo->width, bo->height, DRM_FORMAT_YUV422, handles,
                      pitches, offsets, &bo->fb_id, 0);
  if (ret) {
    printf("drmModeAddFB2 return err %d\n", ret);
    return 0;
  }

  memset(bo->vaddr, 0xff, bo->size);

  return 0;
}

static int modeset_create_planefb(int fd, struct buffer_object *bo) {

  return 0;
}

static void modeset_destroy_fb(int fd, struct buffer_object *bo) {
  struct drm_mode_destroy_dumb destroy = {};

  drmModeRmFB(fd, bo->fb_id);

  munmap(bo->vaddr, bo->size);

  destroy.handle = bo->handle;
  drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

void get_planes_property(int fd, drmModePlaneRes *pr) {
  drmModeObjectPropertiesPtr props;
  int i, j;
  drmModePropertyPtr p;

  for (i = 0; i < pr->count_planes; i++) {

    printf("planes id %d\n", pr->planes[i]);
    props =
        drmModeObjectGetProperties(fd, pr->planes[i], DRM_MODE_OBJECT_PLANE);

    for (j = 0; j < props->count_props; j++) {
      p = drmModeGetProperty(fd, props->props[j]);
      printf("get property ,name %s, id %d\n", p->name, p->prop_id);
      drmModeFreeProperty(p);
    }

    printf("\n\n");
  }
}

void set_plane_property(int fd, int plane_id, struct property_arg *p) {

  drmModeObjectPropertiesPtr props;
  drmModePropertyPtr pt;
  const char *obj_type;
  int ret;
  int i;

  props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

  for (i = 0; i < (int)props->count_props; ++i) {

    pt = drmModeGetProperty(fd, props->props[i]);

    if (!pt)
      continue;
    if (strcmp(pt->name, p->name) == 0)
      break;
  }

  if (i == (int)props->count_props) {
    printf("%i has no %s property\n", p->obj_id, p->name);
    return;
  }

  p->prop_id = props->props[i];

  ret = drmModeObjectSetProperty(fd, p->obj_id, p->obj_type, p->prop_id,
                                 p->value);
  if (ret < 0)
    printf("faild to set property %s,id %d,value %d\n", p->obj_id, p->name,
           p->value);
}

void set_rotation(int fd, int plane_id, int value) {
  struct property_arg prop;

  memset(&prop, 0, sizeof(prop));
  prop.obj_type = 0;
  prop.name[DRM_PROP_NAME_LEN] = '\0';
  prop.obj_id = plane_id;
  memcpy(prop.name, "rotation", sizeof("rotation"));
  prop.value = value; // 1->0^, 2->90^, 3 ->180^,
  set_plane_property(fd, plane_id, &prop);
}

// the alpha is globle setting,e.g. HEO alpha is 255, but still cant cover
// over1,2
void set_alpha(int fd, int plane_id, int value) {
  struct property_arg prop;

  memset(&prop, 0, sizeof(prop));
  prop.obj_type = 0;
  prop.name[DRM_PROP_NAME_LEN] = '\0';
  prop.obj_id = plane_id;
  memcpy(prop.name, "alpha", sizeof("alpha"));
  prop.value = value; // 0~255
  set_plane_property(fd, plane_id, &prop);
}

VideoDecoder *decoder = NULL;
struct ScMemOpsS *memops = NULL;

void init_videocoder() {
  VideoStreamInfo vStreamInfo;
  VConfig vConfig;

  memops = MemAdapterGetOpsS();
  if (memops == NULL) {
    printf("MemAdapterGetOpsS fail\n");
    return NULL;
  }
  CdcMemOpen(memops);
  printf("AddVDPlugin first\n");
  AddVDPlugin();

  decoder = CreateVideoDecoder();
  if (!decoder) {
    printf("create video decoder failed\n");
    return NULL;
  }

  /**init the video decoder parameter**/
  memset(&vStreamInfo, 0, sizeof(VideoStreamInfo));
  memset(&vConfig, 0, sizeof(vConfig));
  vStreamInfo.eCodecFormat = VIDEO_CODEC_FORMAT_MJPEG;
  // vConfig.bDisable3D = 0;
  // vConfig.bDispErrorFrame = 0;
  // vConfig.bNoBFrames = 0;
  // vConfig.bRotationEn = 0;
  // vConfig.bScaleDownEn = 0;
  // vConfig.nHorizonScaleDownRatio = 0;
  // vConfig.nVerticalScaleDownRatio = 0;
  // vConfig.eOutputPixelFormat = PIXEL_FORMAT_YV12;
  vConfig.eOutputPixelFormat = PIXEL_FORMAT_NV12;
  // vConfig.nDeInterlaceHoldingFrameBufferNum = 0;
  // vConfig.nDisplayHoldingFrameBufferNum = 0;
  // vConfig.nRotateHoldingFrameBufferNum = 0;
  // vConfig.nDecodeSmoothFrameBufferNum = 0;
  vConfig.nVbvBufferSize = 1024 * 1024;
  // vConfig.bThumbnailMode = 1;
  vConfig.memops = memops;

  if ((InitializeVideoDecoder(decoder, &vStreamInfo, &vConfig)) != 0) {
    printf("InitializeVideoDecoder failed\n");
    destroy_videocoder();
    return NULL;
  }
}

void destroy_videocoder() {
  if (decoder) {
    DestroyVideoDecoder(decoder);
    decoder = NULL;
  }
  if (memops) {
    CdcMemClose(memops);
    memops = NULL;
  }
}

inline static long long GetNowUs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

#define TARGET_FPS 30
#define BUFFER_SIZE 200 * 1024
#define MAX_SUPPORT_FRAME 3 * 60 * (TARGET_FPS)
#define VIDEOHEIGHT 640
#define VIDEOWIDTH 352

uint32_t *parse_mjpeg_frame_locs(int fd, int *frame_count) {
  uint32_t *frame_locs = malloc(MAX_SUPPORT_FRAME * sizeof(uint32_t));
  uint8_t *buf = malloc(BUFFER_SIZE);

  if (frame_locs == NULL) {
    printf("malloc frame_locs failed\n");
    return NULL;
  }
  memset(frame_locs, 0, MAX_SUPPORT_FRAME * sizeof(uint32_t));
  *frame_count = 0;
  int cur_loc = 0;
  int ret;
  while (1) {
    ret = read(fd, buf, BUFFER_SIZE);
    if (ret < 0) {
      printf("read failed\n");
      return NULL;
    }
    if (ret == 0) {
      break;
    }
    for (int i = 0; i < ret; i++) {
      if (buf[i] == 0xFF && buf[i + 1] == 0xD8 && buf[i + 2] == 0xFF &&
          buf[i + 3] == 0xFE) {
        frame_locs[*frame_count] = cur_loc + i;
        (*frame_count)++;
      }
    }
    cur_loc += ret;
  }
  printf("parsed frame count %d\n", *frame_count);
  lseek(fd, 0, SEEK_SET);
  free(buf);
  return frame_locs;
}

uint32_t read_mjpeg_frame_loc_from_file(int fd, int *frame_count) {
  uint32_t *frame_locs = malloc(MAX_SUPPORT_FRAME * sizeof(uint32_t));

  int filelen = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  *frame_count = 0;

  char linebuf[100];
  char *buf = malloc(filelen);
  read(fd, buf, filelen);

  int lastlinestart = 0;

  for (int i = 0; i < filelen; i++) {
    if (buf[i] == '\n') {
      memcpy(linebuf, buf + lastlinestart, i - lastlinestart);
      linebuf[i - lastlinestart] = '\0';
      frame_locs[*frame_count] = atoi(linebuf);
      (*frame_count)++;
      lastlinestart = i + 1;
    }
  }

  free(buf);
  return frame_locs;
}

int x;
void update_overlay_plane() {
  int16_t x16 = x - 120;
  int ret;
  /* Source values are 16.16 fixed point */
  ret = drmModeSetPlane(
      drm_fd, plane_res->planes[1], crtc_id, plane_buf[1].fb_id, 0,
      (uint16_t)x16, 400, plane_buf[1].width, plane_buf[1].height, 0 << 16,
      0 << 16, (plane_buf[1].width) << 16, (plane_buf[1].height) << 16);
  if (ret < 0)
    printf("drmModeSetPlane err %d\n", ret);

  // usleep(2000);

  // printf("out x:%d y:%d",x,y);

  x += 1;
  //  if(x + plane_buf[2].width >= conn->modes[0].hdisplay){
  if (x >= 240) {
    x = 0;
  }
}

void print_vdecode_status(int ret) {
  switch (ret) {
  case VDECODE_RESULT_KEYFRAME_DECODED:
    printf("VDECODE_RESULT_KEYFRAME_DECODED\n");
    break;
  case VDECODE_RESULT_FRAME_DECODED:
    printf("VDECODE_RESULT_FRAME_DECODED\n");
    break;
  case VDECODE_RESULT_NO_FRAME_BUFFER:
    printf("VDECODE_RESULT_NO_FRAME_BUFFER\n");
    break;
  case VDECODE_RESULT_OK:
    printf("VDECODE_RESULT_OK\n");
    break;
  case VDECODE_RESULT_CONTINUE:
    printf("VDECODE_RESULT_CONTINUE\n");
    break;
  case VDECODE_RESULT_NO_BITSTREAM:
    printf("VDECODE_RESULT_NO_BITSTREAM\n");
    break;
  case VDECODE_RESULT_RESOLUTION_CHANGE:
    printf("VDECODE_RESULT_RESOLUTION_CHANGE\n");
    break;
  case VDECODE_RESULT_UNSUPPORTED:
    printf("VDECODE_RESULT_UNSUPPORTED\n");
    break;
  default:
    printf("VDECODE_RESULT_UNKNOWN\n");
    break;
  }
}

int is_first_frame = 1;

int video_playback_loop(char *filename, uint8_t *outbuf) {

  int decfd = open(filename, O_RDONLY);
  if (decfd < 0) {
    printf("open %s failed\n", filename);
    return -1;
  }
  int videofile_len = lseek(decfd, 0, SEEK_END);
  lseek(decfd, 0, SEEK_SET);

  int frame_count = 0;

  char frame_filename[100];
  sprintf(frame_filename, "%s.txt", filename);
  int frame_fd = open(frame_filename, O_RDONLY);
  if (frame_fd < 0) {
    printf("open %s failed\n", frame_filename);
    return -1;
  }
  uint32_t *frame_locs = read_mjpeg_frame_loc_from_file(frame_fd, &frame_count);
  if (frame_locs == NULL) {
    printf("read frame locs failed\n");
    return -1;
  }
  close(frame_fd);

  // uint32_t* frame_locs = parse_mjpeg_frame_locs(decfd, &frame_count);
  // if(frame_locs == NULL){
  //     printf("parse frame locs failed\n");
  //     return -1;
  // }

  char *buf, *ringBuf;
  int buflen, ringBufLen;
  VideoPicture *videoPicture;
  int availableBufferSize;
  int current_frame_loc = 0;
  int ret;

  uint8_t chroma_buf[VIDEOWIDTH * VIDEOHEIGHT / 2];

  int framecount_in_10s = 0;
  int start_time = time(NULL);

  // Performance measurement variables
  long long t_start, t_end;
  long long time_buffer_request = 0;
  long long time_file_read = 0;
  long long time_submit = 0;
  long long time_decode = 0;
  long long time_copy = 0;
  int perf_sample_count = 0;

  while (1) {
    // Start of frame processing
    long long frame_start = GetNowUs();

    int bufferSize = VideoStreamBufferSize(decoder, 0);
    int dataSize = VideoStreamDataSize(decoder, 0);
    // int frameNum = VideoStreamFrameNum(decoder, 0);
    // int validPictureNum = ValidPictureNum(decoder, 0);
    // printf("bufferSize is %d, dataSize is %d, frameNum is %d, validPictureNum
    // is %d\n", bufferSize, dataSize, frameNum, validPictureNum);

    availableBufferSize = bufferSize - dataSize;
    int framesize =
        frame_locs[current_frame_loc + 1] - frame_locs[current_frame_loc];
    if (framesize > availableBufferSize) {
      printf("framesize is too large, availableBufferSize is %d\n",
             availableBufferSize);
      goto trydecode;
    }

    /**request vbm buffer from video decoder**/
    t_start = GetNowUs();
    ret = RequestVideoStreamBuffer(decoder, framesize, (char **)&buf, &buflen,
                                   (char **)&ringBuf, &ringBufLen, 0);
    t_end = GetNowUs();
    time_buffer_request += (t_end - t_start);

    if (ret < 0) {
      printf("Request Video Stream Buffer failed\n");
      goto trydecode;
    }
    if (buflen + ringBufLen < framesize) {
      printf("#####Error: request buffer failed, buffer is not enough\n");
      return -1;
    }

    // printf("copying frame %d to video decoder,frame size is %d,buffer size is
    // %d,ringbuf size is %d\n", current_frame_loc, framesize, buflen,
    // ringBufLen);

    /**copy and submit stream to video decoder SBM**/
    // PERFORMANCE CRITICAL: File I/O operations (lseek + read)
    t_start = GetNowUs();

    // Measure lseek separately
    long long t_lseek_start = GetNowUs();
    lseek(decfd, frame_locs[current_frame_loc], SEEK_SET);
    long long t_lseek_end = GetNowUs();
    long long lseek_time = t_lseek_end - t_lseek_start;

    // Measure read operations separately
    long long t_read1_start = GetNowUs();
    long long read1_bytes = 0;
    long long read2_bytes = 0;
    long long read1_time = 0;
    long long read2_time = 0;

    if (buflen >= framesize) {
      read1_bytes = read(decfd, buf, framesize);
      long long t_read1_end = GetNowUs();
      read1_time = t_read1_end - t_read1_start;
    } else {
      read1_bytes = read(decfd, buf, buflen);
      long long t_read1_end = GetNowUs();
      read1_time = t_read1_end - t_read1_start;

      long long t_read2_start = GetNowUs();
      read2_bytes = read(decfd, ringBuf, framesize - buflen);
      long long t_read2_end = GetNowUs();
      read2_time = t_read2_end - t_read2_start;
    }

    t_end = GetNowUs();
    long long total_io_time = t_end - t_start;
    time_file_read += total_io_time;

    // Print detailed timing breakdown if file I/O is slow (>10ms)
    if (total_io_time > 10000) {
      printf("[SLOW I/O] Frame %d: TOTAL=%lld us | lseek=%lld us | read1=%lld "
             "us (%lld bytes) | read2=%lld us (%lld bytes)\\n",
             current_frame_loc, total_io_time, lseek_time, read1_time,
             read1_bytes, read2_time, read2_bytes);
    }

    VideoStreamDataInfo DataInfo;
    memset(&DataInfo, 0, sizeof(DataInfo));
    DataInfo.pData = buf;
    DataInfo.nLength = framesize;
    DataInfo.bIsFirstPart = is_first_frame;
    DataInfo.bIsLastPart = 0;
    is_first_frame = 0;

    t_start = GetNowUs();
    if (SubmitVideoStreamData(decoder, &DataInfo, 0)) {
      printf("#####Error: Submit Video Stream Data failed!\n");
      return -1;
    }
    t_end = GetNowUs();
    time_submit += (t_end - t_start);

    /**decode stream**/
    int endofstream = 0;
    int dropBFrameifdelay = 0;
    int64_t currenttimeus = 0;
    int decodekeyframeonly = 0;
  trydecode:

    t_start = GetNowUs();
    ret = DecodeVideoStream(decoder, endofstream, decodekeyframeonly,
                            dropBFrameifdelay, currenttimeus);
    t_end = GetNowUs();
    time_decode += (t_end - t_start);

    // print_vdecode_status(ret);
    switch (ret) {
    case VDECODE_RESULT_KEYFRAME_DECODED:
    case VDECODE_RESULT_FRAME_DECODED: {
      ret = ValidPictureNum(decoder, 0);
      if (ret >= 0) {
        videoPicture = RequestPicture(decoder, 0);
        if (videoPicture == NULL) {
          // printf("RequestPicture fail\n");
          goto sleepandnext;
        }
        // printf("decoder one pic...\n");
        // printf("pic nWidth is %d,nHeight is
        // %d\n",videoPicture->nWidth,videoPicture->nHeight);
        // printf("videoPicture->nWidth = %d,videoPicture->nHeight =
        // %d,videoPicture->nLineStride =
        // %d\n",videoPicture->nWidth,videoPicture->nHeight,videoPicture->nLineStride);
        // printf("videoPicture->nTopOffset = %d,videoPicture->nLeftOffset =
        // %d,videoPicture->nBottomOffset = %d,videoPicture->nRightOffset = %d",
        //         videoPicture->nTopOffset,videoPicture->nLeftOffset,videoPicture->nBottomOffset,videoPicture->nRightOffset);

        int nwidth = videoPicture->nWidth;
        int nheight = videoPicture->nHeight;
        /* flush cache*/
        // CdcMemFlushCache(memops, videoPicture->pData0, nwidth*nheight);
        // CdcMemFlushCache(memops, videoPicture->pData1, nheight*nheight/4);
        // CdcMemFlushCache(memops, videoPicture->pData2, nheight*nheight/4);
        /**copy actural picture data to imgframe**/
        // memcpy(outbuf,videoPicture->pData0,nwidth*nheight);
        // memcpy(outbuf+nwidth*nheight,videoPicture->pData1,nwidth*nheight/4);
        // memcpy(outbuf+nwidth*nheight*5/4,videoPicture->pData2,nwidth*nheight/4);

        // memcpy(chroma_buf,videoPicture->pData1,nwidth*nheight/4);
        // memcpy(chroma_buf+nwidth*nheight/4,videoPicture->pData2,nwidth*nheight/4);

        // // yuvmb32_2_rgb565(nwidth, nheight, outbuf, videoPicture->pData0,
        // chroma_buf); mb32dec(nwidth, nheight, outbuf, videoPicture->pData0,
        // chroma_buf);

        t_start = GetNowUs();
        memcpy(outbuf, videoPicture->pData0, nwidth * nheight);
        memcpy(outbuf + nwidth * nheight, videoPicture->pData1,
               nwidth * nheight / 2);
        t_end = GetNowUs();
        time_copy += (t_end - t_start);

        // printf("returning picture to video decoder\n");
        /**return the picture buf to video decoder**/
        ReturnPicture(decoder, videoPicture);

      } else {
        printf("no ValidPictureNum ret is %d\n", ret);
      }
      break;
    }

    case VDECODE_RESULT_OK:
    case VDECODE_RESULT_CONTINUE:
    case VDECODE_RESULT_NO_BITSTREAM:
    case VDECODE_RESULT_NO_FRAME_BUFFER:
    case VDECODE_RESULT_RESOLUTION_CHANGE:
    case VDECODE_RESULT_UNSUPPORTED:
    default:
      break;
    }
  sleepandnext:
    // update_overlay_plane();
    // usleep(1000);
    current_frame_loc++;
    if (current_frame_loc >= frame_count - 1) {
      printf("all frames decoded back from beginning\n");
      current_frame_loc = 0;
    }
    framecount_in_10s++;
    perf_sample_count++;

    // Print performance statistics every 10 seconds
    if (time(NULL) - start_time >= 10) {
      printf("\n========== PERFORMANCE REPORT (10 seconds) ==========\n");
      printf("Frames processed: %d (%.1f fps)\n", framecount_in_10s,
             framecount_in_10s / 10.0);

      if (perf_sample_count > 0) {
        printf("\nAverage time per operation (microseconds):\n");
        printf("  File Read (lseek+read):  %6lld us  (%.1f%%)\n",
               time_file_read / perf_sample_count,
               100.0 * time_file_read /
                   (time_file_read + time_buffer_request + time_submit +
                    time_decode + time_copy));
        printf("  Buffer Request:          %6lld us  (%.1f%%)\n",
               time_buffer_request / perf_sample_count,
               100.0 * time_buffer_request /
                   (time_file_read + time_buffer_request + time_submit +
                    time_decode + time_copy));
        printf("  Submit Stream Data:      %6lld us  (%.1f%%)\n",
               time_submit / perf_sample_count,
               100.0 * time_submit /
                   (time_file_read + time_buffer_request + time_submit +
                    time_decode + time_copy));
        printf("  Decode Stream:           %6lld us  (%.1f%%)\n",
               time_decode / perf_sample_count,
               100.0 * time_decode /
                   (time_file_read + time_buffer_request + time_submit +
                    time_decode + time_copy));
        printf("  Copy Picture Data:       %6lld us  (%.1f%%)\n",
               time_copy / perf_sample_count,
               100.0 * time_copy /
                   (time_file_read + time_buffer_request + time_submit +
                    time_decode + time_copy));
        printf("  TOTAL per frame:         %6lld us\n",
               (time_file_read + time_buffer_request + time_submit +
                time_decode + time_copy) /
                   perf_sample_count);
      }
      printf("====================================================\n\n");

      // Reset counters
      framecount_in_10s = 0;
      start_time = time(NULL);
      time_buffer_request = 0;
      time_file_read = 0;
      time_submit = 0;
      time_decode = 0;
      time_copy = 0;
      perf_sample_count = 0;
    }
  }
  return 0;
}

uint8_t *init_defe_layer0(int fd, int width, int height, uint8_t **vaddr_out,
                          int *fb_id_out) {
  struct drm_mode_create_dumb creq = {0};
  struct drm_mode_destroy_dumb dreq = {0};
  struct drm_mode_map_dumb mreq = {0};
  uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
  uint64_t modifiers[4] = {0};
  int ret;

  printf("set creq in\n");

  creq.width = width;
  creq.height = height * 3 / 2;
  ;
  creq.bpp = 8;

  printf("creq out\n");

  ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
  if (ret < 0) {
    fprintf(stderr, "cannot create dumb buffer (%d): %m\n", errno);
    return -errno;
  }

  printf("ioctl out\n");

  offsets[0] = 0;
  handles[0] = creq.handle;
  pitches[0] = creq.pitch;
  modifiers[0] = DRM_FORMAT_MOD_ALLWINNER_TILED;

  offsets[1] = creq.pitch * height;
  handles[1] = creq.handle;
  pitches[1] = creq.pitch;
  modifiers[1] = DRM_FORMAT_MOD_ALLWINNER_TILED;

  int fb_id;

  ret = drmModeAddFB2WithModifiers(fd, width, height, DRM_FORMAT_NV12, handles,
                                   pitches, offsets, modifiers, &fb_id,
                                   DRM_MODE_FB_MODIFIERS);
  // ret = drmModeAddFB2WithModifiers(fd, width, height,
  //     DRM_FORMAT_NV12, handles, pitches, offsets,modifiers,&fb_id, 0);
  printf("drm add in\n");

  // ret = drmModeAddFB2(fd, width, height,
  //     DRM_FORMAT_RGB565, handles, pitches, offsets,&fb_id, 0);

  if (ret) {
    printf("drmModeAddFB2 return err %d\n", ret);
    return 0;
  }

  printf("drm add out\n");

  /* prepare buffer for memory mapping */
  memset(&mreq, 0, sizeof(mreq));
  mreq.handle = creq.handle;
  ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
  if (ret) {
    fprintf(stderr, "1st cannot map dumb buffer (%d): %m\n", errno);
    return -1;
  }

  uint8_t *vaddr;
  /* perform actual memory mapping */
  vaddr = mmap(0, width * height * 3 / 2, PROT_READ | PROT_WRITE, MAP_SHARED,
               fd, mreq.offset);
  if (vaddr == MAP_FAILED) {
    fprintf(stderr, "2nd cannot mmap dumb buffer (%d): %m\n", errno);
    return -1;
  }
  *vaddr_out = vaddr;
  *fb_id_out = fb_id;
  return 0;
}

int main(int argc, char **argv) {
  int fd;
  uint32_t conn_id;
  uint32_t plane_id;
  int ret;
  int rotation = 1, alpha = 10;
  ;

  init_videocoder();

  fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  drm_fd = fd;

  res = drmModeGetResources(fd);
  crtc_id = res->crtcs[0];
  conn_id = res->connectors[0];

  drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret) {
    printf("failed to set client cap\n");
    return -1;
  }
  plane_res = drmModeGetPlaneResources(fd);
  plane_id = plane_res->planes[0];

  printf("get plane count %d,plane_id %d\n", plane_res->count_planes, plane_id);

  conn = drmModeGetConnector(fd, conn_id);
  buf.width = conn->modes[0].hdisplay;
  buf.height = conn->modes[0].vdisplay;

  printf("get connector nanme %s,hdisplay %d, vdisplay %d,vrefresh %d\n",
         conn->modes[0].name, conn->modes[0].vdisplay, conn->modes[0].hdisplay,
         conn->modes[0].vrefresh);
  //  modeset_create_fb(fd, &buf);
  //  drmModeSetCrtc(fd, crtc_id, buf.fb_id,0, 0, &conn_id, 1, &conn->modes[0]);

  //  ret = drmModeSetPlane(fd, plane_res->planes[0], crtc_id, buf.fb_id, 0,
  //     0, 0, buf.width,buf.height,
  //     0, 0, (buf.width) << 16, (buf.height) << 16);
  // if(ret < 0)
  //     printf("drmModeSetPlane err %d\n",ret);

  // getchar();

  //  printf("primary plane\n");

  //  plane_buf[0].width = conn->modes[0].hdisplay;
  //  plane_buf[0].height = conn->modes[0].vdisplay;
  //  modeset_create_fb(fd, &plane_buf[0], DRM_FORMAT_XRGB8888);
  //  write_color(&plane_buf[0],0x00ff00ff);

  int img_width, img_height;
  uint8_t *img_data;

  // img_data= load_image_BGRA8888("bg.png",&img_width,&img_height);
  // printf("img_width %d,img_height %d\n",img_width,img_height);

  //  plane_buf[0].width = img_width;
  //  plane_buf[0].height = img_height;
  //  modeset_create_fb(fd, &plane_buf[0], DRM_FORMAT_RGB565,16);
  //  // BGRA8888 to RGB565

  //  for (int i = 0; i < img_width*img_height; i++) {
  //     int b = img_data[i*4+2];
  //     int g = img_data[i*4+1];
  //     int r = img_data[i*4+0];
  //     ((uint16_t *)plane_buf[0].vaddr)[i] = (r>>3)<<11 | (g>>2)<<5 | (b>>3);
  //  }
  // free(img_data);

  // int decfd = open("testimg352640.jpeg",O_RDONLY);
  // if(decfd < 0) {
  //    printf("open testimg352640.jpeg failed\n");
  //    return -1;
  // }
  // int jpgbuf_len = lseek(decfd, 0, SEEK_END);
  // lseek(decfd, 0, SEEK_SET);
  // uint8_t* jpgbuf = malloc(jpgbuf_len);
  // read(decfd, jpgbuf, jpgbuf_len);
  // close(decfd);

  plane_buf[0].width = VIDEOWIDTH;
  plane_buf[0].height = VIDEOHEIGHT;
  // modeset_create_fb(fd, &plane_buf[0], DRM_FORMAT_VYUY,16,0);

  ret =
      init_defe_layer0(fd, 352, 640, &plane_buf[0].vaddr, &plane_buf[0].fb_id);
  if (ret < 0)
    printf("init_defe_layer0 err %d\n", ret);

  printf("lets start JPEG DECODING!!!\n");

  // int declen = plane_buf[0].width * plane_buf[0].height * 3 / 2;
  // uint8_t* inbuf = malloc(declen);
  // jpeg2yuv(jpgbuf, jpgbuf_len, inbuf);
  // free(jpgbuf);
  // yuvmb32_2_rgb565(plane_buf[0].width, plane_buf[0].height,
  // plane_buf[0].vaddr, inbuf, inbuf + plane_buf[0].width *
  // plane_buf[0].height); free(inbuf);

  ret =
      drmModeSetPlane(fd, plane_res->planes[0], crtc_id, plane_buf[0].fb_id, 0,
                      4, 0, plane_buf[0].width, plane_buf[0].height, 0, 0,
                      (plane_buf[0].width) << 16, (plane_buf[0].height) << 16);
  if (ret < 0)
    printf("drmModeSetPlane err %d\n", ret);

  //  // -------------------  overlay 1

  printf("overlay1\n");

  //  plane_buf[1].width = 200;
  //  plane_buf[1].height = 200;
  //  modeset_create_fb(fd, &plane_buf[1]);
  //  write_color(&plane_buf[1],0x00ff0000);

  img_data = load_image_BGRA8888(OVERLAY_FILE_NAME, &img_width, &img_height);
  printf("img_width %d,img_height %d\n", img_width, img_height);

  plane_buf[1].width = img_width;
  plane_buf[1].height = img_height;
  modeset_create_fb(fd, &plane_buf[1], DRM_FORMAT_ARGB8888, 32, 0);
  // BGRA8888 to RGB565

  for (int i = 0; i < img_width * img_height; i++) {
    plane_buf[1].vaddr[i * 4] = img_data[i * 4 + 2];
    plane_buf[1].vaddr[i * 4 + 1] = img_data[i * 4 + 1];
    plane_buf[1].vaddr[i * 4 + 2] = img_data[i * 4 + 0];
    plane_buf[1].vaddr[i * 4 + 3] = img_data[i * 4 + 3];
  }
  free(img_data);

  ret = drmModeSetPlane(fd, plane_res->planes[1], crtc_id, plane_buf[1].fb_id,
                        0, (360 - img_width) / 2, 10, plane_buf[1].width,
                        plane_buf[1].height, 0, 0, (plane_buf[1].width) << 16,
                        (plane_buf[1].height) << 16);
  if (ret < 0)
    printf("drmModeSetPlane err %d\n", ret);

  // getchar();

  video_playback_loop(VIDEO_FILE_NAME, plane_buf[0].vaddr);

  //  ret = drmModeSetPlane(fd, plane_res->planes[1], crtc_id,
  //  plane_buf[1].fb_id, 0,
  //          0, 0, plane_buf[1].width,plane_buf[1].height,
  //          0, 0, (plane_buf[1].width) << 16, (plane_buf[1].height) << 16);
  //  if(ret < 0)
  //      printf("drmModeSetPlane err %d\n",ret);

  //  printf("overlay2\n");
  //  // -------------------  overlay 2
  //  plane_buf[2].width = 200;
  //  plane_buf[2].height = 200;
  //  modeset_create_fb(fd, &plane_buf[2]);
  //  write_color(&plane_buf[2],0x0000ff00);

  //  ret = drmModeSetPlane(fd, plane_res->planes[2], crtc_id,
  //  plane_buf[2].fb_id, 0,
  //          100, 100, plane_buf[2].width,plane_buf[2].height,
  //          0, 0, plane_buf[2].width << 16, plane_buf[2].height << 16);
  //  if(ret < 0)
  //      printf("drmModeSetPlane err %d\n",ret);

  //  get_planes_property(fd,plane_res);

  // printf("heo\n");
  // // -------------------  HEO
  // plane_buf[2].width = 200;
  // plane_buf[2].height = 200;
  // modeset_create_fb(fd, &plane_buf[2]);
  // write_color_half(&plane_buf[2],0x000000ff,0x00000000);

  // img_data =
  // load_image_BGRA8888("rd_title_small.png",&img_width,&img_height);
  // printf("img_width %d,img_height %d\n",img_width,img_height);

  //  printf("overlay3\n");
  //  // -------------------  overlay 3
  //  plane_buf[3].width = img_width;
  //  plane_buf[3].height = img_height;
  //  modeset_create_fb(fd, &plane_buf[3], DRM_FORMAT_XRGB8888,32,0);
  //  // BGRA8888 to ARGB8888

  //  for (int i = 0; i < img_width*img_height; i++) {
  //     plane_buf[3].vaddr[i*4] = img_data[i*4+2];
  //     plane_buf[3].vaddr[i*4+1] = img_data[i*4+1];
  //     plane_buf[3].vaddr[i*4+2] = img_data[i*4+0];
  //     plane_buf[3].vaddr[i*4+3] = img_data[i*4+3];
  //  }
  // free(img_data);

  //  ret = drmModeSetPlane(fd, plane_res->planes[3], crtc_id,
  //  plane_buf[3].fb_id, 0,
  //          200, 200, plane_buf[3].width,plane_buf[3].height,
  //          0, 0, plane_buf[3].width << 16, plane_buf[3].height << 16);
  //  if(ret < 0)
  //      printf("drmModeSetPlane err %d\n",ret);

  // printf("press any key continue\n");
  // getchar();

  // x = 0;
  // y = 0;
  // while(1){
  //     // printf("in x:%d y:%d",x,y);
  //    int16_t x16 = x - 120;
  //     /* Source values are 16.16 fixed point */
  //     ret = drmModeSetPlane(fd, plane_res->planes[1], crtc_id,
  //     plane_buf[1].fb_id, 0,
  //            x16, y, plane_buf[1].width,plane_buf[1].height,
  //             0<<16, 0<<16,
  //             (plane_buf[1].width) << 16, (plane_buf[1].height) << 16);
  //     if(ret < 0)
  //         printf("drmModeSetPlane err %d\n",ret);

  //     usleep(2000);

  //     // printf("out x:%d y:%d",x,y);

  //     x += 1;
  //    //  if(x + plane_buf[2].width >= conn->modes[0].hdisplay){
  //        if(x  >= 360){
  //         x = 0;
  //         y += 50;
  //         printf("x:%d y:%d\n",x,y);
  //        //  if(y + plane_buf[2].height >= conn->modes[0].vdisplay)
  //         if(y  >= 110)
  //             y = 0;

  //     }

  // }

  modeset_destroy_fb(fd, &buf);

  drmModeFreeConnector(conn);
  drmModeFreePlaneResources(plane_res);
  drmModeFreeResources(res);

  close(fd);

  return 0;
}
