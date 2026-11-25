/* libScePngDec and libSceJpegDec: still images decoded with FFmpeg (already linked for video) and
 * converted with swscale into the game's buffer. Headers are parsed by hand. A decoder handle
 * holds no state: every decode opens its own codec context, so handles can be used from any thread. */
#define _GNU_SOURCE
#include "runtime.h"
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>

#define PNG_INVALID_ADDR ((int32_t)0x80690001)
#define PNG_INVALID_SIZE ((int32_t)0x80690002)
#define PNG_INVALID_PARAM ((int32_t)0x80690003)
#define PNG_INVALID_HANDLE ((int32_t)0x80690004)
#define PNG_INVALID_DATA ((int32_t)0x80690010)
#define PNG_DECODE_ERROR ((int32_t)0x80690012)
#define JPEG_INVALID_ADDR ((int32_t)0x80650001)
#define JPEG_INVALID_SIZE ((int32_t)0x80650002)
#define JPEG_INVALID_PARAM ((int32_t)0x80650003)
#define JPEG_INVALID_HANDLE ((int32_t)0x80650004)
#define JPEG_EXCEED_MAX_WIDTH ((int32_t)0x80650006)
#define JPEG_INVALID_DATA ((int32_t)0x80650010)
#define JPEG_DECODE_ERROR ((int32_t)0x80650012)

#define PNG_MAGIC 0x504e4744u     /* "PNGD" */
#define JPEG_MAGIC 0x4a504744u    /* "JPGD" */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }

/* Decodes one image and writes it as width x height pixels of format at dst with pitch bytes per
 * row. alpha >= 0 replaces the alpha of images that have none. Returns 0, or -1 on bad data. */
static int decode_image(enum AVCodecID codec_id,const uint8_t *data,uint32_t size,uint8_t *dst,uint32_t pitch,
                        uint32_t dst_size,enum AVPixelFormat format,int width,int height,int alpha,
                        int *source_width,int *source_height) {
    const AVCodec *codec=avcodec_find_decoder(codec_id);
    AVCodecContext *context=codec ? avcodec_alloc_context3(codec) : NULL;
    AVPacket *packet=av_packet_alloc();
    AVFrame *frame=av_frame_alloc();
    int result=-1;
    if (!context || !packet || !frame) goto done;
    context->thread_count=1;
    if (avcodec_open2(context,codec,NULL)<0) goto done;
    packet->data=(uint8_t *)data; packet->size=(int)size;
    if (avcodec_send_packet(context,packet)<0 || avcodec_receive_frame(context,frame)<0) goto done;
    if (source_width) *source_width=frame->width;
    if (source_height) *source_height=frame->height;
    if (width<=0) width=frame->width;
    if (height<=0) height=frame->height;
    if ((uint64_t)pitch*(uint64_t)(height-1)+(uint64_t)width*(format==AV_PIX_FMT_GRAY8 ? 1 : 4)>dst_size) goto done;
    /* JPEG decodes to the full-range "J" formats, which swscale wants spelled as range, not format. */
    enum AVPixelFormat source_format=(enum AVPixelFormat)frame->format;
    int full_range=frame->color_range==AVCOL_RANGE_JPEG;
    switch (source_format) {
    case AV_PIX_FMT_YUVJ420P: source_format=AV_PIX_FMT_YUV420P; full_range=1; break;
    case AV_PIX_FMT_YUVJ422P: source_format=AV_PIX_FMT_YUV422P; full_range=1; break;
    case AV_PIX_FMT_YUVJ444P: source_format=AV_PIX_FMT_YUV444P; full_range=1; break;
    case AV_PIX_FMT_YUVJ440P: source_format=AV_PIX_FMT_YUV440P; full_range=1; break;
    default: break;
    }
    struct SwsContext *sws=sws_getContext(frame->width,frame->height,source_format,
                                          width,height,format,SWS_BILINEAR,NULL,NULL,NULL);
    if (!sws) goto done;
    if (full_range) {
        const int *coefficients=sws_getCoefficients(SWS_CS_ITU601);
        sws_setColorspaceDetails(sws,coefficients,1,coefficients,1,0,1<<16,1<<16);
    }
    uint8_t *planes[4]={dst}; int strides[4]={(int)pitch};
    sws_scale(sws,(const uint8_t *const *)frame->data,frame->linesize,0,frame->height,planes,strides);
    sws_freeContext(sws);
    const AVPixFmtDescriptor *source=av_pix_fmt_desc_get((enum AVPixelFormat)frame->format);
    const int has_alpha=source && (source->flags&AV_PIX_FMT_FLAG_ALPHA || source->flags&AV_PIX_FMT_FLAG_PAL);
    if (alpha>=0 && !has_alpha && format!=AV_PIX_FMT_GRAY8)
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) dst[(size_t)y*pitch+(size_t)x*4+3]=(uint8_t)alpha;
    result=0;
done:
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&context);
    return result;
}

/* ---- PNG ---- */

typedef struct { const uint8_t *data; uint32_t size, reserved; } PngParseParam;
typedef struct { uint32_t width, height; uint16_t color_space, bit_depth; uint32_t flags; } PngImageInfo;
typedef struct { uint32_t this_size, attribute, max_width; } PngCreateParam;
typedef struct {
    const uint8_t *data; uint8_t *image;
    uint32_t data_size, image_size;
    uint16_t pixel_format, alpha; uint32_t pitch;
} PngDecodeParam;

/* IHDR, then any tRNS before the image data. */
static int32_t png_header(const uint8_t *data,uint32_t size,PngImageInfo *info) {
    static const uint8_t signature[8]={0x89,'P','N','G','\r','\n',0x1a,'\n'};
    if (!data) return PNG_INVALID_ADDR;
    if (size<33 || memcmp(data,signature,8) || memcmp(data+12,"IHDR",4)) return PNG_INVALID_DATA;
    const uint8_t color=data[25];
    PngImageInfo out={be32(data+16),be32(data+20),0,data[24],0};
    switch (color) {
    case 0: out.color_space=2; break;        /* grayscale */
    case 2: out.color_space=3; break;        /* RGB */
    case 3: out.color_space=4; break;        /* palette */
    case 4: out.color_space=18; break;       /* grayscale and alpha */
    case 6: out.color_space=19; break;       /* RGBA */
    default: return PNG_INVALID_DATA;
    }
    if (data[28]==1) out.flags|=1;           /* Adam7 */
    for (uint32_t at=8;at+12<=size;) {
        const uint32_t length=be32(data+at);
        if (!memcmp(data+at+4,"IDAT",4) || !memcmp(data+at+4,"IEND",4)) break;
        if (!memcmp(data+at+4,"tRNS",4)) { out.flags|=2; break; }
        if (length>size-at-12) break;
        at+=12+length;
    }
    if (info) *info=out;
    return 0;
}

static ABI int32_t png_query_memory_size(const PngCreateParam *param) {
    if (!param) return PNG_INVALID_PARAM;
    if (param->attribute>1) return PNG_INVALID_ADDR;
    if (param->max_width-1>1000000) return PNG_INVALID_SIZE;
    return 64;
}

static ABI int32_t png_create(const PngCreateParam *param,void *memory,uint32_t memory_size,void **handle) {
    if (!param || param->attribute>1) return PNG_INVALID_PARAM;
    if (!memory || !handle) return PNG_INVALID_ADDR;
    if (param->max_width-1>1000000 || memory_size<sizeof(uint32_t)) return PNG_INVALID_SIZE;
    *(uint32_t *)memory=PNG_MAGIC; *handle=memory;
    return 0;
}

static ABI int32_t png_delete(void *handle) {
    if (!handle || *(uint32_t *)handle!=PNG_MAGIC) return PNG_INVALID_HANDLE;
    *(uint32_t *)handle=0; return 0;
}

static ABI int32_t png_parse_header(const PngParseParam *param,PngImageInfo *info) {
    if (!param) return PNG_INVALID_PARAM;
    return png_header(param->data,param->size,info);
}

static ABI int32_t png_decode(void *handle,const PngDecodeParam *param,PngImageInfo *info) {
    if (!handle || *(uint32_t *)handle!=PNG_MAGIC) return PNG_INVALID_HANDLE;
    if (!param) return PNG_INVALID_PARAM;
    if (!param->data || !param->image) return PNG_INVALID_ADDR;
    PngImageInfo header;
    int32_t r=png_header(param->data,param->data_size,&header);
    if (r) return r;
    if (info) *info=header;
    const uint32_t pitch=param->pitch ? param->pitch : header.width*4;
    const enum AVPixelFormat format=param->pixel_format==1 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
    if (decode_image(AV_CODEC_ID_PNG,param->data,param->data_size,param->image,pitch,param->image_size,format,
                     (int)header.width,(int)header.height,param->alpha&0xff,NULL,NULL))
        return PNG_DECODE_ERROR;
    return header.width>32767 || header.height>32767 ? 0 : (int32_t)(header.width<<16|header.height);
}

/* ---- JPEG ---- */

typedef struct { void *self; uint32_t handle_size, decode_mode, max_width, reserved; } JpegHandle;
typedef struct { uint32_t size, decode_mode, max_width; } JpegCreateParam;
typedef struct { const uint8_t *data; uint32_t size; uint16_t decode_mode, ratio; } JpegParseParam;
typedef struct {
    uint32_t width, height;
    uint16_t color_space, components;
    uint8_t component_id[4], sampling[4];
    uint32_t decode_mode, required_decode_mode, output_width, output_height;
} JpegImageInfo;
_Static_assert(sizeof(JpegImageInfo)==0x24,"SceJpegDecImageInfo");
typedef struct {
    const uint8_t *data; uint8_t *image; void *aux;
    uint32_t data_size, image_size, aux_size;
    uint16_t decode_mode, ratio, pixel_format, alpha;
    uint32_t pitch;
} JpegDecodeParam;
_Static_assert(sizeof(JpegDecodeParam)==0x30,"SceJpegDecDecodeParam");

static int ratio_valid(uint32_t ratio) { return ratio==1 || ratio==2 || ratio==4 || ratio==8; }

/* The frame header (SOF0-2): size, components and their sampling. */
static int32_t jpeg_header(const uint8_t *data,uint32_t size,uint32_t decode_mode,uint32_t ratio,JpegImageInfo *info) {
    if (!data) return JPEG_INVALID_ADDR;
    if (!size) return JPEG_INVALID_SIZE;
    if (decode_mode>1 || !ratio_valid(ratio)) return JPEG_INVALID_PARAM;
    if (size<4 || data[0]!=0xff || data[1]!=0xd8) return JPEG_INVALID_DATA;
    JpegImageInfo out={0};
    out.components=3;
    for (uint32_t at=2;at+4<size;) {
        if (data[at]!=0xff) { ++at; continue; }
        const uint8_t marker=data[at+1];
        if (marker==0xd9 || marker==0xda) break;
        if (marker==0xff || marker==0x01 || (marker>=0xd0 && marker<=0xd7)) { at+=marker==0xff ? 1 : 2; continue; }
        const uint32_t length=(uint32_t)data[at+2]<<8|data[at+3];
        if (marker>=0xc0 && marker<=0xc2) {
            if (at+9>=size) break;
            out.height=(uint32_t)data[at+5]<<8|data[at+6];
            out.width=(uint32_t)data[at+7]<<8|data[at+8];
            out.components=data[at+9];
            for (uint32_t c=0;c<out.components && c<4 && at+11+c*3<size;++c) {
                out.component_id[c]=data[at+10+c*3]; out.sampling[c]=data[at+11+c*3];
            }
            break;
        }
        at+=2+length;
    }
    if (!out.width || !out.height) return JPEG_DECODE_ERROR;
    out.color_space=out.components==1 ? 2 : out.components==3 ? 1 : 0;
    /* Decode mode 2 is needed for sampling denser than 4:2:0 (the shadPS4 rule). */
    out.required_decode_mode=1;
    if (out.color_space==1) {
        uint32_t max_h=1, density=0;
        for (uint32_t c=0;c<3;++c) {
            if ((uint32_t)(out.sampling[c]>>4)>max_h) max_h=out.sampling[c]>>4;
            density+=(uint32_t)(out.sampling[c]>>4)*(out.sampling[c]&15);
        }
        out.required_decode_mode=density*(96/(max_h*8))<0x25 ? 1 : 2;
    }
    out.decode_mode=decode_mode;
    out.output_width=(out.width+ratio-1)/ratio; out.output_height=(out.height+ratio-1)/ratio;
    if (info) *info=out;
    return 0;
}

static ABI int32_t jpeg_query_memory_size(const JpegCreateParam *param) {
    if (!param) return JPEG_INVALID_ADDR;
    if (param->size!=sizeof(*param)) return JPEG_INVALID_SIZE;
    if (param->decode_mode<1 || param->decode_mode>2 || !param->max_width || param->max_width>0xffff)
        return JPEG_INVALID_PARAM;
    const uint32_t width=((param->max_width+0x1f)&~0x1fu)|0x10;
    const uint32_t work=param->decode_mode==2 ? (width<<5)*5 : (width<<4)*3;
    return (int32_t)(work+width*6+0x7360);
}

static ABI int32_t jpeg_create(const JpegCreateParam *param,void *memory,uint32_t memory_size,JpegHandle **handle) {
    if (!param || !memory || !handle) return JPEG_INVALID_ADDR;
    const int32_t needed=jpeg_query_memory_size(param);
    if (needed<0) return needed;
    if ((uint32_t)needed>memory_size) return JPEG_INVALID_SIZE;
    JpegHandle *decoder=(JpegHandle *)(((uintptr_t)memory+0x1f)&~(uintptr_t)0x1f);
    *decoder=(JpegHandle){decoder,sizeof(*param),param->decode_mode,(param->max_width+0x1f)&~0x1fu,JPEG_MAGIC};
    *handle=decoder;
    return 0;
}

static int jpeg_handle_valid(const JpegHandle *handle) {
    return handle && !((uintptr_t)handle&0x1f) && handle->self==handle && handle->reserved==JPEG_MAGIC;
}

static ABI int32_t jpeg_delete(JpegHandle *handle) {
    if (!jpeg_handle_valid(handle)) return JPEG_INVALID_HANDLE;
    handle->self=NULL; return 0;
}

static ABI int32_t jpeg_parse_header(const JpegParseParam *param,JpegImageInfo *info) {
    if (!param || !info) return JPEG_INVALID_ADDR;
    return jpeg_header(param->data,param->size,param->decode_mode,param->ratio,info);
}

static ABI int32_t jpeg_decode(JpegHandle *handle,const JpegDecodeParam *param,JpegImageInfo *info) {
    if (!jpeg_handle_valid(handle)) return JPEG_INVALID_HANDLE;
    if (!param || !param->data || !param->image) return JPEG_INVALID_ADDR;
    if (!param->data_size || !param->image_size) return JPEG_INVALID_SIZE;
    /* Pixel formats: RGBA8 (0), BGRA8 (1), Y8 (11). Y8U8Y8V8 (10) is not decoded. */
    if (param->pixel_format!=0 && param->pixel_format!=1 && param->pixel_format!=11) return JPEG_INVALID_PARAM;
    if (param->alpha>0xff) return JPEG_INVALID_PARAM;
    JpegImageInfo header;
    int32_t r=jpeg_header(param->data,param->data_size,param->decode_mode,param->ratio,&header);
    if (r) return r;
    if (header.width>handle->max_width) return JPEG_EXCEED_MAX_WIDTH;
    const uint32_t bpp=param->pixel_format==11 ? 1 : 4, min_pitch=header.output_width*bpp;
    const uint32_t pitch=param->pitch ? param->pitch : min_pitch;
    if (pitch<min_pitch || (bpp==4 && pitch&3)) return JPEG_INVALID_PARAM;
    if ((uint64_t)pitch*header.output_height>param->image_size) return param->pitch ? JPEG_INVALID_PARAM : JPEG_INVALID_SIZE;
    const enum AVPixelFormat format=param->pixel_format==11 ? AV_PIX_FMT_GRAY8
                                  : param->pixel_format==1 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
    if (decode_image(AV_CODEC_ID_MJPEG,param->data,param->data_size,param->image,pitch,param->image_size,format,
                     (int)header.output_width,(int)header.output_height,param->alpha ? param->alpha : 0xff,NULL,NULL))
        return JPEG_DECODE_ERROR;
    if (info) *info=header;
    return (int32_t)(header.output_width<<16|(header.output_height&0xffff));
}

static const RuntimeExport exports[]={
    {"scePngDecQueryMemorySize",png_query_memory_size}, {"scePngDecCreate",png_create},
    {"scePngDecDelete",png_delete}, {"scePngDecParseHeader",png_parse_header}, {"scePngDecDecode",png_decode},
    {"sceJpegDecQueryMemorySize",jpeg_query_memory_size}, {"sceJpegDecCreate",jpeg_create},
    {"sceJpegDecDelete",jpeg_delete}, {"sceJpegDecParseHeader",jpeg_parse_header},
    {"sceJpegDecDecode",jpeg_decode},
};
uintptr_t runtime_image_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
