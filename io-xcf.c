/*
 * Pixbuf loader for xcf
 *
 * Author(s):
 *	Stephane Delcroix  <stephane@delcroix.org>
 *
 * Copyright (C) 2009 Novell, Inc
 *
 * This is a clean room implementation, based solely on  
 * http://henning.makholm.net/xcftools/xcfspec.txt and hexdumps
 * of existing .xcf files.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * TODO:
 * - read xcf.gz, xcf.bz2
 * - fix and test on bigendian machines
 * - fix the spots/stains (where are they coming from) ?
 * - indexed mode
 * - if the bg layer mode is not Normal or Dissolve, change it to Normal
 * - file an enhancement request to gdk-pixbuf
 */

#define GDK_PIXBUF_ENABLE_BACKEND

#include <gmodule.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-io.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <bzlib.h>

#define LOG(...) printf (__VA_ARGS__);

//FIXME: change this macro to noop on bigendian machines
#define SWAP(int32) ( ((int32) >> 24) + \
		      ((int32) >> 8 & 0x0000FF00 )+ \
		      ((int32) << 8 & 0x00FF0000 )+ \
		      ((int32) << 24) )

#define PROP_END 		0
#define PROP_COLORMAP 		1
#define PROP_FLOATING_SELECTION	5
#define PROP_OPACITY		6
#define PROP_MODE		7
#define PROP_VISIBLE		8
#define PROP_LINKED		9
#define PROP_APPLY_MASK		11
#define PROP_OFFSETS		15
#define PROP_COMPRESSION 	17
#define PROP_GUIDES 		18
#define PROP_RESOLUTION 	19
#define PROP_TATOO		20
#define PROP_PARASITES		21
#define PROP_UNIT 		22
#define PROP_PATHS 		23
#define PROP_USER_UNIT 		24
#define PROP_VECTORS		25

#define COMPRESSION_NONE	0
#define COMPRESSION_RLE		1

#define LAYERTYPE_RGB		0
#define LAYERTYPE_RGBA		1
#define LAYERTYPE_GRAYSCALE	2
#define LAYERTYPE_GRAYSCALEA	3
#define LAYERTYPE_INDEXED	4
#define LAYERTYPE_INDEXEDA	5

#define LAYERMODE_NORMAL	0
#define LAYERMODE_DISSOLVE	1
#define LAYERMODE_BEHIND	2
#define LAYERMODE_MULTIPLY	3
#define LAYERMODE_SCREEN	4
#define LAYERMODE_OVERLAY	5
#define LAYERMODE_DIFFERENCE	6
#define LAYERMODE_ADDITION	7
#define LAYERMODE_SUBTRACT	8
#define LAYERMODE_DARKENONLY	9
#define LAYERMODE_LIGHTENONLY	10
#define LAYERMODE_HUE		11
#define LAYERMODE_SATURATION	12
#define LAYERMODE_COLOR		13
#define LAYERMODE_VALUE		14
#define LAYERMODE_DIVIDE	15
#define LAYERMODE_DODGE		16
#define LAYERMODE_BURN		17
#define LAYERMODE_HARDLIGHT	18
#define LAYERMODE_SOFTLIGHT	19
#define LAYERMODE_GRAINEXTRACT	20
#define LAYERMODE_GRAINMERGE	21

#define FILETYPE_UNKNOWN	0
#define FILETYPE_XCF		1
#define FILETYPE_XCF_BZ2	2

typedef struct _XcfContext XcfContext;
struct _XcfContext {
	GdkPixbufModuleSizeFunc size_func;
	GdkPixbufModulePreparedFunc prepare_func;
	GdkPixbufModuleUpdatedFunc update_func;
	gpointer user_data;
	gint type;
	bz_stream *bz_stream;

	gchar *tempname;
	FILE *file;
};

typedef struct _XcfChannel XcfChannel;
struct _XcfChannel {
	guint32 width;
	guint32 height;
	gboolean visible;
	guint32 opacity;
	guint32 lptr;
};

typedef struct _XcfLayer XcfLayer;
struct _XcfLayer {
	guint32	width;
	guint32	height;
	guint32 type;
	guint32 mode;
	gboolean apply_mask;
	gboolean visible;
	guint32 opacity;
	gint32 dx;
	gint32 dy;
	XcfChannel* layer_mask;
	guint32 lptr;;
};

void
rle_decode (FILE *f, gchar *ptr, int count, int type)
{
	int channels;
	switch (type) {
		case LAYERTYPE_RGB : channels = 3; break;
		case LAYERTYPE_RGBA: channels = 4; break;
		case LAYERTYPE_GRAYSCALE: channels = 1; break;
		case LAYERTYPE_GRAYSCALEA: channels = 2; break;
		case LAYERTYPE_INDEXED: channels = 1; break;
		case LAYERTYPE_INDEXEDA: channels = 2; break;
	}

	guchar opcode;
	guchar buffer[3];
	guchar ch[channels][count];
	int channel;

	//un-rle
	for (channel = 0; channel < channels; channel++) {
		int pixels_count = 0;
		while (pixels_count < count) {
			fread (&opcode, sizeof(guchar), 1, f);
			if (opcode <= 126) {
				fread (buffer, 1, 1, f);
				opcode ++;
				while (opcode --) 
					memcpy (ch[channel] + (pixels_count++), buffer, 1);
			} else if (opcode == 127) {
				fread (buffer, 3, 1, f);
				int p = buffer[0];
				int q = buffer[1];
				int count = p*256+q;
				while (count --)
					memcpy (ch[channel] + (pixels_count++), buffer+2, 1);
			} else if (opcode == 128) {
				fread (buffer, 2, 1, f);
				int p = buffer[0];
				int q = buffer[1];
				fread (ch[channel] + pixels_count, p*256+q, 1, f);
				pixels_count += p*256+q;
			} else if (opcode >= 129) {
				fread (ch[channel] + pixels_count, 256 - opcode, 1, f);
				pixels_count += 256 - opcode;	
			}
		}
	}

	//reinterlace the channels
	int i, j;
	for (i=0; i <count; i++)
		for (j=0; j<channels; j++)
			memcpy (ptr + i * channels + j, ch[j] + i, 1);
}

void
to_rgba (gchar *ptr, int count, int type)
{
	//pad to rgba
	int i;

	for (i=count-1; i>=0;i--)
		switch (type) {
		case LAYERTYPE_RGB:
			memcpy (ptr + 4*i, ptr + 3*i, 3);
			ptr[4*i + 3] = 0xff;
			break;
		case LAYERTYPE_RGBA:
			///nothing to do
			break;
		case LAYERTYPE_GRAYSCALE:
			memcpy (ptr + 4*i, ptr + i, 1);
			memcpy (ptr + 4*i + 1, ptr + i, 1);
			memcpy (ptr + 4*i + 2, ptr + i, 1);
			ptr[4*i + 3] = 0xff;
			break;
		case LAYERTYPE_GRAYSCALEA: 
			memcpy (ptr + 4*i, ptr + i, 1);
			memcpy (ptr + 4*i + 1, ptr + i, 1);
			memcpy (ptr + 4*i + 2, ptr + i, 1);
			memcpy (ptr + 4+i + 3, ptr + i + 1, 1);
			break;
		}
}

void
apply_opacity (guchar* ptr, int size, guint32 opacity)
{
	int i;
	for (i=0; i<size; i++)
		ptr[4*i + 3] = (guchar)((ptr[4*i+3] * opacity) / 0xff);
}

void
apply_mask (FILE *f, gchar compression, guchar *ptr, int size, XcfChannel *mask, int tile_id)
{
	//save file position
	long pos = ftell (f);
	
	guint32 tptr = mask->lptr + (2 + tile_id) * sizeof(guint32); //skip width and height
	fseek (f, tptr, SEEK_SET);
	fread (&tptr, sizeof(guint32), 1, f);
	fseek (f, SWAP(tptr), SEEK_SET);

	gchar pixels[4096];
	if (compression == COMPRESSION_RLE)
		rle_decode (f, pixels, size, LAYERTYPE_GRAYSCALE);
	else //COMPRESSION_NONE
		fread (pixels, sizeof(gchar), size, f);

	int i;
	for (i = 0; i<size; i++)
		ptr[4*i + 3] = ptr[4 * i + 3] * pixels[i] / 0xff;

	//rewind
	fseek (f, pos, SEEK_SET);
}


void
intersect_tile (guchar* ptr, int im_width, int im_height, int *ox, int *oy, int *tw, int *th)
{
	int i;
	if (*ox < 0) {
		for (i=0; i<*th; i++) {
			memmove (ptr + 4 * i * (*tw + *ox), ptr + 4 * i * (*tw), 4 * (*tw + *ox));
		}
		*tw = *tw + *ox;
		*ox = 0;
	}
	if (*oy < 0) {
		memmove (ptr, ptr + 4 * *tw * -*oy, 4 * *tw * (*th + *oy));
		*th = *th + *oy;
		*oy = 0;
	}
	if (*ox + *tw > im_width) {
		for (i=0; i<*th; i++) {
			memmove (ptr + 4 * i * (im_width - *ox), ptr + 4 * i * (*tw), 4 * (im_width - *ox));
		}
		*tw = im_width - *ox;
	}
	if (*oy + *th > im_height) {
		*th = im_height - *oy;
	}
}

void blend (guchar* rgba0, guchar* rgba1)
{
	guchar k = 0xff * rgba1[3] / (0xff - (0xff-rgba0[3])*(0xff-rgba1[3])/0xff);
	rgba0[0] = ((0xff - k) * rgba0[0] + k * rgba1[0]) / 0xff;
	rgba0[1] = ((0xff - k) * rgba0[1] + k * rgba1[1]) / 0xff;
	rgba0[2] = ((0xff - k) * rgba0[2] + k * rgba1[2]) / 0xff;	
}

typedef void (*composite_func) (guchar* rgb0, guchar* rgb1);

void
multiply (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = (rgb0[0] * rgb1[0] ) / 0xff;
	rgb1[1] = (rgb0[1] * rgb1[1] ) / 0xff;
	rgb1[2] = (rgb0[2] * rgb1[2] ) / 0xff;
}

void
screen (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = 0xff - (0xff - rgb0[0]) * (0xff - rgb1[0]) / 0xff;
	rgb1[1] = 0xff - (0xff - rgb0[1]) * (0xff - rgb1[1]) / 0xff;
	rgb1[2] = 0xff - (0xff - rgb0[2]) * (0xff - rgb1[2]) / 0xff;
}

void
overlay (guchar *rgb0, guchar *rgb1)
{
	//LOG ("Overlay (%d %d %d) (%d %d %d) : ", rgb0[0], rgb0[1], rgb0[2], rgb1[0], rgb1[1], rgb1[2]);
	rgb1[0] = MIN (0xff, ((0xff - rgb1[0]) * rgb0[0] * rgb0[0] / 0xff + rgb0[0] * (0xff - (0xff - rgb1[0]) * (0xff - rgb1[0]) / 0xff)) / 0xff);
	rgb1[1] = MIN (0xff, ((0xff - rgb1[1]) * rgb0[1] * rgb0[1] / 0xff + rgb0[1] * (0xff - (0xff - rgb1[1]) * (0xff - rgb1[1]) / 0xff)) / 0xff);
	rgb1[2] = MIN (0xff, ((0xff - rgb1[2]) * rgb0[2] * rgb0[2] / 0xff + rgb0[2] * (0xff - (0xff - rgb1[2]) * (0xff - rgb1[2]) / 0xff)) / 0xff);
	//LOG ("(%d %d %d)\n", rgb1[0], rgb1[1], rgb1[2]);
}

void
difference (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = (rgb0[0] > rgb1[0]) ? rgb0[0] - rgb1[0] : rgb1[0] - rgb0[0];
	rgb1[1] = (rgb0[1] > rgb1[1]) ? rgb0[1] - rgb1[1] : rgb1[1] - rgb0[1];
	rgb1[2] = (rgb0[2] > rgb1[2]) ? rgb0[2] - rgb1[2] : rgb1[2] - rgb0[2];
}

void
addition (guchar *rgb0, guchar *rgb1)
{
//	LOG ("addition (%d %d %d) (%d %d %d):", rgb0[0], rgb0[1], rgb0[2], rgb1[0], rgb1[1], rgb1[2]);
	rgb1[0] = (rgb0[0] + rgb1[0]) > 0xff ? 0xff : rgb0[0] + rgb1[0];
	rgb1[1] = (rgb0[1] + rgb1[1]) > 0xff ? 0xff : rgb0[1] + rgb1[1];
	rgb1[2] = (rgb0[2] + rgb1[2]) > 0xff ? 0xff : rgb0[2] + rgb1[2];
//	LOG ("(%d %d %d)\n", rgb1[0], rgb1[1], rgb1[2]);
}

void
subtract (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = (rgb0[0] - rgb1[0]) < 0 ? 0 : rgb0[0] - rgb1[0];
	rgb1[1] = (rgb0[1] - rgb1[1]) < 0 ? 0 : rgb0[1] - rgb1[1];
	rgb1[2] = (rgb0[2] - rgb1[2]) < 0 ? 0 : rgb0[2] - rgb1[2];
}

void
min (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = MIN (rgb0[0], rgb1[0]);
	rgb1[1] = MIN (rgb0[1], rgb1[1]);
	rgb1[2] = MIN (rgb0[2], rgb1[2]);
}

void
max (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = MAX (rgb0[0], rgb1[0]);
	rgb1[1] = MAX (rgb0[1], rgb1[1]);
	rgb1[2] = MAX (rgb0[2], rgb1[2]);
}

void
divide (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = rgb1[0] == 0 ? (rgb0[0] == 0 ? 0 : 0xff) : MIN (0xff, 0xff * rgb0[0] / rgb1[0]);
	rgb1[1] = rgb1[1] == 0 ? (rgb0[1] == 0 ? 0 : 0xff) : MIN (0xff, 0xff * rgb0[1] / rgb1[1]);
	rgb1[2] = rgb1[2] == 0 ? (rgb0[2] == 0 ? 0 : 0xff) : MIN (0xff, 0xff * rgb0[2] / rgb1[2]);
}

void
dodge (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = rgb1[0] == 0xff ? (rgb0[0] == 0 ? 0 : 0xff) : MIN (0xff, 0xff * rgb0[0] / (0xff - rgb1[0]));
	rgb1[1] = rgb1[1] == 0xff ? (rgb0[1] == 0 ? 0 : 0xff) : MIN (0xff, 0xff * rgb0[1] / (0xff - rgb1[1]));
	rgb1[2] = rgb1[2] == 0xff ? (rgb0[2] == 0 ? 0 : 0xff) : MIN (0xff, 0xff * rgb0[2] / (0xff - rgb1[2]));
}

void
burn (guchar *rgb0, guchar *rgb1) // 1-(1-x1)/x2
{
	rgb1[0] = rgb1[0] == 0 ? (rgb0[0] == 0xff ? 0xff : 0) : 0xff - MIN (0xff, 0xff * (0xff - rgb0[0]) / rgb1[0]);
	rgb1[1] = rgb1[1] == 0 ? (rgb0[1] == 0xff ? 0xff : 0) : 0xff - MIN (0xff, 0xff * (0xff - rgb0[1]) / rgb1[1]);
	rgb1[2] = rgb1[2] == 0 ? (rgb0[2] == 0xff ? 0xff : 0) : 0xff - MIN (0xff, 0xff * (0xff - rgb0[2]) / rgb1[2]);
}

void
hardlight (guchar *rgb0, guchar *rgb1) //if x2 < 0.5 then 2*x1*x2 else 1-2*(1-x1)(1-x2)
{
	rgb1[0] = rgb1[0] < 0x80 ? 2 * rgb0[0] * rgb1[0] / 0xff : 0xff - 2 * (0xff - rgb0[0]) * (0xff - rgb1[0])/ 0xff;
	rgb1[1] = rgb1[1] < 0x80 ? 2 * rgb0[1] * rgb1[1] / 0xff : 0xff - 2 * (0xff - rgb0[1]) * (0xff - rgb1[1])/ 0xff;
	rgb1[2] = rgb1[2] < 0x80 ? 2 * rgb0[2] * rgb1[2] / 0xff : 0xff - 2 * (0xff - rgb0[2]) * (0xff - rgb1[2])/ 0xff;
}

void
grainextract (guchar *rgb0, guchar *rgb1) //x1-x2+.5
{
	rgb1[0] = MAX (0, MIN (0xff, rgb0[0] - rgb1[0] + 0x80));
	rgb1[1] = MAX (0, MIN (0xff, rgb0[1] - rgb1[1] + 0x80));
	rgb1[2] = MAX (0, MIN (0xff, rgb0[2] - rgb1[2] + 0x80));
}

void
grainmerge (guchar *rgb0, guchar *rgb1)
{
	rgb1[0] = MAX (0, MIN (0xff, rgb0[0] + rgb1[0] - 0x80));
	rgb1[1] = MAX (0, MIN (0xff, rgb0[1] + rgb1[1] - 0x80));
	rgb1[2] = MAX (0, MIN (0xff, rgb0[2] + rgb1[2] - 0x80));
}


//FIXME: any way to do the following 4 ones in integer arithmetic ?
void
hue (guchar *rgb0, guchar *rgb1)
{
	if (rgb1[0] == rgb1[1] == rgb1[2]) {
		rgb1[0] = rgb0[0];
		rgb1[1] = rgb0[1];
		rgb1[2] = rgb0[2];
		return;
	}
	//hue of rgb1, value and saturation of rgb0
	guchar min0 = MIN (MIN (rgb0[0], rgb0[1]), rgb0[2]);
	guchar max0 = MAX (MAX (rgb0[0], rgb0[1]), rgb0[2]);
	guchar min1 = MIN (MIN (rgb1[0], rgb1[1]), rgb1[2]);
	guchar max1 = MAX (MAX (rgb1[0], rgb1[1]), rgb1[2]);
	if (max0 == 0) {
		rgb1[0] = 0x00;
		rgb1[1] = 0x00;
		rgb1[2] = 0x00;
		return;
	}
	double p = max0 * (max0 - min0) / (max1*(max0-min0) - min1*max0 + max1*min0);
	double q = - max0 * (min1*max0 - max1*min0) / (max1*(max0-min0) - min1*max0 + max1*min0);
	rgb1[0] = (guchar)(rgb1[0] * p + q);
	rgb1[1] = (guchar)(rgb1[1] * p + q);
	rgb1[2] = (guchar)(rgb1[2] * p + q);
}

void
saturation (guchar *rgb0, guchar *rgb1)
{
	//hue and value of rgb0, saturation of rgb1
	guchar min0 = MIN (MIN (rgb0[0], rgb0[1]), rgb0[2]);
	guchar max0 = MAX (MAX (rgb0[0], rgb0[1]), rgb0[2]);
	guchar min1 = MIN (MIN (rgb1[0], rgb1[1]), rgb1[2]);
	guchar max1 = MAX (MAX (rgb1[0], rgb1[1]), rgb1[2]);
	if (max0 == 0) {
		rgb1[0] = 0x00;
		rgb1[1] = 0x00;
		rgb1[2] = 0x00;
		return;
	}
	if (max0 == min0) {
		rgb1[0] = max0;
		rgb1[1] = min1*max0 / max0;
		rgb1[2] = rgb1[1];
		return;
	}
	double p = max0 * (min1 - max1) / (max0*(min1-max1) - min1*max0 + max1*min0);
	double q = - max0 * (min1*max0 - max1*min0) / (max0*(min1-max1) - min1*max0 + max1*min0);
	rgb1[0] = (guchar)(rgb0[0] * p + q);
	rgb1[1] = (guchar)(rgb0[1] * p + q);
	rgb1[2] = (guchar)(rgb0[2] * p + q);
	
}

void
value (guchar *rgb0, guchar *rgb1)
{
	//hue and saturation ov rgb0, value of rgb1
	guchar min0 = MIN (MIN (rgb0[0], rgb0[1]), rgb0[2]);
	guchar max0 = MAX (MAX (rgb0[0], rgb0[1]), rgb0[2]);
	guchar min1 = MIN (MIN (rgb1[0], rgb1[1]), rgb1[2]);
	guchar max1 = MAX (MAX (rgb1[0], rgb1[1]), rgb1[2]);
	if (max0 == 0) {
		rgb1[0] = 0x00;
		rgb1[1] = 0x00;
		rgb1[2] = 0x00;
		return;
	}
	if (max0 == min0) {
		rgb1[0] = max1;
		rgb1[1] = max1;
		rgb1[2] = max1;
		return;
	}

	double p = max1 / max0;

	rgb1[0] = (guchar)(rgb0[0] * p);
	rgb1[1] = (guchar)(rgb0[1] * p);
	rgb1[2] = (guchar)(rgb0[2] * p);
}

void
color (guchar *rgb0, guchar *rgb1)
{
	//hue and hsl-saturation or rgb1, luminosity of rgb0
	guchar min0 = MIN (MIN (rgb0[0], rgb0[1]), rgb0[2]);
	guchar max0 = MAX (MAX (rgb0[0], rgb0[1]), rgb0[2]);
	guchar min1 = MIN (MIN (rgb1[0], rgb1[1]), rgb1[2]);
	guchar max1 = MAX (MAX (rgb1[0], rgb1[1]), rgb1[2]);

	double p = MIN ((min0+max0)/2, 0xff - (min0+max0)/2) / MIN ((min1+max1)/2, 0xff - (min1+max1)/2);
	double q = (min0 + max0 - (min1 + max1) * p) / 2.0;

	rgb1[0] = (guchar)(rgb1[0] * p + q);
	rgb1[1] = (guchar)(rgb1[1] * p + q);
	rgb1[2] = (guchar)(rgb1[2] * p + q);
}

void
composite (gchar *pixbuf_pixels, int rowstride, gchar *tile_pixels, int ox, int oy, int tw, int th, guint32 layer_mode)
{
	composite_func f = NULL;
	int origin = 4 * ox + rowstride * oy;
	int i, j;

	switch (layer_mode) {
	case LAYERMODE_NORMAL:
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				//a0 = 1 - (1-a0)*(a-a1)
				//rgb0 = BLEND (rgba0, rgba1)
				gchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				gchar *src = tile_pixels + j*tw*4 + i*4;
				guchar alpha = 0xff - (0xff - dest[3]) * (0xff - src [3]);
				blend (dest, src);
				dest[3] = alpha;
			}
		break;
	case LAYERMODE_DISSOLVE:
		srand(time(0));
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				guchar d = rand () % 0x100;
				dest [0] = d <= src[3] ? src[0] : dest[0];
				dest [1] = d <= src[3] ? src[1] : dest[1];
				dest [2] = d <= src[3] ? src[2] : dest[2];
				dest [3] = d <= src[3] ? 0xff : dest[3];
			}
		break;
	case LAYERMODE_BEHIND: //ignore
		break;
	// 3<=mode<=10 || 15<=mode<=21
	// a0 = a0
	// rgba0 = blend (rgba0, F(rgb0, rgb1), MIN(a0, a1)
	case LAYERMODE_MULTIPLY:
		f = multiply;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_SCREEN:
		f = screen;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_OVERLAY:
	case LAYERMODE_SOFTLIGHT:
		f = overlay;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_DIFFERENCE:
		f = difference;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_ADDITION:
		f = addition;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_SUBTRACT:
		f = subtract;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_DARKENONLY:
		f = min;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_LIGHTENONLY:
		f = max;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_DIVIDE:
		f = divide;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_DODGE:
		f = dodge;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_BURN:
		f = burn;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_HARDLIGHT:
		f = hardlight;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_GRAINEXTRACT:
		f = grainextract;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_GRAINMERGE:
		f = grainmerge;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_HUE:
		f = hue;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_SATURATION:
		f = saturation;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_VALUE:
		f = value;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	case LAYERMODE_COLOR:
		f = color;
		for (j=0;j<th;j++)
			for (i=0;i<tw;i++) {
				guchar *dest = pixbuf_pixels + origin + j * rowstride + 4 * i;
				guchar *src = tile_pixels + j*tw*4 + i*4;
				f (dest, src);
				src[3] = MIN (dest[3], src[3]);
				blend (dest, src);
			}
		break;
	
	default:	//Pack layer on top of each other, without any blending at all
		for (j=0; j<th;j++) {
			memcpy (pixbuf_pixels + origin + j * rowstride, tile_pixels + j*tw*4 , tw*4);
		}
		break;
	}

}

static GdkPixbuf*
xcf_image_load_real (FILE *f, XcfContext *context, GError **error)
{
	guint32 width;
	guint32 height;
	guint32 color_mode;
	gchar compression = 0;
	GList *layers = NULL;
	GdkPixbuf *pixbuf = NULL;

	guchar buffer[32];
	guint32 data[3];
	guint32 property[2];

	//Magic and version
	fread (buffer, sizeof(guchar), 9, f);
	//LOG ("%s\n", buffer);
	if (strncmp (buffer, "gimp xcf ", 9)) {
		g_set_error_literal (error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_CORRUPT_IMAGE, "Wrong magic");
		return NULL;
	}

	fread (buffer, sizeof(guchar), 4, f);
	if (strncmp (buffer, "file", 4) && strncmp (buffer, "v001", 4) && strncmp (buffer, "v002", 4)) {
		g_set_error_literal (error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE, "Unsupported version");
		return NULL;
	}
	fread (buffer, sizeof(guchar), 1, f);
	
	//Canvas size and Color mode
	fread (data, sizeof(guint32), 3, f);

	width = SWAP(data[0]);
	height = SWAP(data[1]);
	color_mode = SWAP(data[2]);
	if (color_mode == 2) { //Indexed, not supported for now
		g_set_error_literal (error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_UNKNOWN_TYPE, "Indexed color mode unsupported");
		return NULL;
	}


	LOG ("W: %d, H: %d, mode: %d\n", width, height, color_mode);
		
	//Image Properties
	while (1) {
		fread (property, sizeof(guint32), 2, f); //read property and payload
		if (!property[0])
			break;
		property[0] = SWAP(property[0]);
		property[1] = SWAP(property[1]);
		//LOG ("property %d, payload %d\n", property[0], property[1]);
		switch (property[0]) {
		case PROP_COMPRESSION:
			fread (&compression, sizeof(gchar), 1, f);
			LOG ("compression: %d\n", compression);
			break;
		case PROP_COLORMAP: //essential, need to parse this
		case PROP_END:
		default:
			//skip the payload
			fseek (f, property[1], SEEK_CUR);
			break;
		}
	}

	//Layer Pointer
	guint32 layer_ptr;
	while (1) {
		fread (&layer_ptr, sizeof(guint32), 1, f);
		layer_ptr = SWAP (layer_ptr);
		if (!layer_ptr)
			break;;

		XcfLayer *layer = g_try_new (XcfLayer, 1);
		if (!layer) {
			g_set_error_literal (error,
			     GDK_PIXBUF_ERROR,
			     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
			     "Cannot allocate memory for loading XCF image");
			return NULL;
		}

		gboolean ignore_layer = FALSE;

		layer->mode = 0;
		layer->apply_mask = FALSE;
		layer->layer_mask = NULL;
		layer->dx = layer->dy = 0;
		layer->visible = TRUE;
		layer->opacity = 0xff;

		//LOG ("layer_ptr: %d\n", layer_ptr);
		long pos = ftell (f);
		//jump to the layer
		fseek(f, layer_ptr, SEEK_SET);
		
		//layer width, height, type
		fread (data, sizeof(guint32), 3, f);
		layer->width = SWAP(data[0]);
		layer->height = SWAP(data[1]);
		layer->type = SWAP(data[2]);
		LOG("\tLayer w:%d h:%d type:%d\n", layer->width, layer->height, layer->type);

		//Layer name, ignore
		guint32 string_size;
		fread (&string_size, sizeof(guint32), 1, f);
		fseek (f, SWAP(string_size), SEEK_CUR);

		//Layer properties
		while (1) {
			fread (property, sizeof(guint32), 2, f); //property and payload
			if (!property[0])
				break;		//break on PROP_END
			property[0] = SWAP (property[0]);
			property[1] = SWAP (property[1]);
			//LOG ("\tproperty %d, payload %d\n", property[0], property[1]);
			switch (property[0]) {
			case PROP_OPACITY:
				fread (data, sizeof(guint32), 1, f);
				layer->opacity = SWAP(data[0]);
				break;
			case PROP_MODE:
				fread (data, sizeof(guint32), 1, f);
				layer->mode = SWAP (data[0]);
				break;
			case PROP_VISIBLE:
				fread (data, sizeof(guint32), 1, f);
				if (SWAP(data[0]) == 0) {
					layer->visible = FALSE;
					ignore_layer = TRUE;
				}
				break;
			case PROP_APPLY_MASK:
				fread (data, sizeof(guint32), 1, f);
				if (SWAP(data[0]) == 1)
					layer->apply_mask = TRUE;
				break;
			case PROP_OFFSETS:
				fread(data, sizeof(gint32), 2, f);
				layer->dx = SWAP(data[0]);
				layer->dy = SWAP(data[1]);
				break;
			case PROP_FLOATING_SELECTION:
				ignore_layer = TRUE;
			default:
				//skip the payload
				fseek (f, property[1], SEEK_CUR);
				break;
			}
		}

		//Hierararchy Pointer
		guint32 hptr;
		fread (&hptr, sizeof(guint32), 1, f);
		hptr = SWAP (hptr);
		long pos1 = ftell (f);
		//jump to hierarchy
		fseek (f, hptr, SEEK_SET);

		//Hierarchy w, h, bpp
		fread (data, sizeof(guint32), 3, f);
		data[0] = SWAP(data[0]);
		data[1] = SWAP(data[1]);
		data[2] = SWAP(data[2]);
		//LOG ("\tHierarchy w:%d, h:%d, bpp:%d\n", data[0], data[1], data[2]);

		guint32 lptr;
		fread (&lptr, sizeof(guint32), 1, f);
		layer->lptr = SWAP (lptr);
		//Layer parsing is done at rendering time

		//Here I could iterate over the unused dlevels and skip them

		//rewind to the layer position
		fseek (f, pos1, SEEK_SET);

		//Mask Pointer
		guint32 mptr;
		fread (&mptr, sizeof(guint32), 1, f);
		if (mptr)
			mptr = SWAP(mptr);

		//rewind to the previous position
		fseek (f, pos, SEEK_SET);

		
		if (!ignore_layer)
			layers = g_list_prepend (layers, layer); //prepend so the layers are in a bottom-up order in the list
		else {
			g_free (layer);
			continue;
		}

		if (!layer->apply_mask || !mptr)
			continue;
		
		LOG ("\t\tthis layer has a mask\n");
		XcfChannel *mask = g_try_new (XcfChannel, 1);
		if (!mask) {
			g_set_error_literal (error,
			     GDK_PIXBUF_ERROR,
			     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
			     "Cannot allocate memory for loading XCF image");
			return NULL;
		}

		mask->opacity = 0xff;
		mask->visible = TRUE;

		//LOG ("\t\tchannel_ptr: %d\n", mptr);
		long mpos = ftell (f);
		//jump to the channel
		fseek(f, mptr, SEEK_SET);

		//Channel w, h
		fread (data, sizeof(guint32), 2, f);
		data[0] = SWAP(data[0]);
		data[1] = SWAP(data[1]);
		LOG ("\t\tChannel w:%d, h:%d\n", data[0], data[1]);

		//Channel name, ignore
		fread (&string_size, sizeof(guint32), 1, f);
		fseek (f, SWAP(string_size), SEEK_CUR);

		//Channel properties
		while (1) {
			fread (property, sizeof(guint32), 2, f); //property and payload
			if (!property[0])
				break;		//break on PROP_END
			property[0] = SWAP (property[0]);
			property[1] = SWAP (property[1]);
			//LOG ("\tproperty %d, payload %d\n", property[0], property[1]);
			switch (property[0]) {
			case PROP_OPACITY:
				fread (data, sizeof(guint32), 1, f);
				mask->opacity = SWAP(data[0]);
				break;
			case PROP_VISIBLE:
				fread (data, sizeof(guint32), 1, f);
				if (SWAP(data[0]) == 0)
					mask->visible = FALSE;
				break;
			default:
				//skip the payload
				fseek (f, property[1], SEEK_CUR);
				break;
			}
		}

		//Hierararchy Pointer
		fread (&hptr, sizeof(guint32), 1, f);
		hptr = SWAP (hptr);
		long mpos1 = ftell (f);
		//jump to hierarchy
		fseek (f, hptr, SEEK_SET);

		//Hierarchy w, h, bpp
		fread (data, sizeof(guint32), 3, f);
		data[0] = SWAP(data[0]);
		data[1] = SWAP(data[1]);
		data[2] = SWAP(data[2]);
		//LOG ("\tHierarchy w:%d, h:%d, bpp:%d\n", data[0], data[1], data[2]);

		fread (&lptr, sizeof(guint32), 1, f);
		mask->lptr = SWAP (lptr);
		//level parsing is done at render time

		if (mask->visible)
			layer->layer_mask = mask;
		else
			g_free (mask);

		//rewind...
		fseek (f, mpos1, SEEK_SET);

		//rewind to the previous position
		fseek (f, mpos, SEEK_SET);
	}

	//Channels goes here, don't read

	LOG("Done parsing\n");

	//Compose the pixbuf
	pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
	LOG ("pixbuf %d %d\n", gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf));
	LOG ("PrepareFunc\n");
	if (context && context->prepare_func)
		(* context->prepare_func) (pixbuf, NULL, context->user_data);

	if (!pixbuf)
		g_set_error_literal (error,
				     GDK_PIXBUF_ERROR,
				     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
				     "Cannot allocate memory for loading XCF image");	

	gdk_pixbuf_fill (pixbuf, 0x00000000);

	GList *current;
	gchar pixels[16384];
	for (current = g_list_first (layers); current; current = g_list_next(current)) {
		XcfLayer *layer = current->data;
		if (!layer->visible)
			continue;

		fseek (f, layer->lptr, SEEK_SET);
		//Ignore Level w and h (same as hierarchy)
		fseek (f, 2 * sizeof(guint32), SEEK_CUR);


		//Iterate on the tiles
		guint32 tptr;
		int tile_id = 0;
		guchar *pixs = gdk_pixbuf_get_pixels (pixbuf);
		int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
		int line_width = ceil ((layer->width) / 64.0);

		while (1) {
			fread (&tptr, sizeof(guint32), 1, f);
			if (!tptr)
				break;
			tptr = SWAP(tptr);
			long lpos = ftell (f);
			fseek (f, tptr, SEEK_SET);

			int ox = 64 * (tile_id % line_width);
			int oy = 64 * (tile_id / line_width);
			int tw = MIN (64, layer->width - ox);
			int th = MIN (64, layer->height - oy);
			ox += layer->dx;
			oy += layer->dy;
			//LOG("\tTile %d %d (%d %d) (%d %d)\n", tile_id, tptr, ox, oy, tw, th);

			//if the tile doesn't intersect with the canvas, ignore
			if (ox + tw < 0 || oy + th < 0 || ox > (int)width || oy > (int)height ) {
				fseek (f, lpos, SEEK_SET);
				tile_id++;
				continue;
			}

			//decompress
			if (compression == COMPRESSION_RLE)
				rle_decode (f, pixels, tw*th, layer->type);
			else {//COMPRESSION_NONE
				int channels;
				switch (layer->type) {
					case LAYERTYPE_RGB : channels = 3; break;
					case LAYERTYPE_RGBA: channels = 4; break;
					case LAYERTYPE_GRAYSCALE: channels = 1; break;
					case LAYERTYPE_GRAYSCALEA: channels = 2; break;
					case LAYERTYPE_INDEXED: channels = 1; break;
					case LAYERTYPE_INDEXEDA: channels = 2; break;
				}
				fread (pixels, sizeof(gchar), tw*th*channels, f);
			}

			//pad to rgba
			to_rgba (pixels, tw*th, layer->type);

			//apply mask
			if (layer->layer_mask)
				apply_mask (f, compression, pixels, tw*th, layer->layer_mask, tile_id);

			//reduce the tile to its intersection with the canvas
			intersect_tile (pixels, width, height, &ox, &oy, &tw, &th);
			
			//apply layer opacity
			apply_opacity (pixels, tw*th, layer->opacity);

			//composite
			composite (pixs, rowstride, pixels, ox, oy, tw, th, layer->mode);
			
			//notify
			if (context && context->update_func)
				(* context->update_func) (pixbuf, ox, oy, tw, th, context->user_data);


			fseek (f, lpos, SEEK_SET);
			tile_id++;
		}
	}

	//free the layers and masks
	for (current = g_list_first (layers); current; current = g_list_next(current)) {
		XcfLayer *layer = current->data;
		if (layer->layer_mask)
			g_free (layer->layer_mask);
	}
	g_list_free (layers);

	return pixbuf;
}

/* Static Loader */

static GdkPixbuf*
xcf_image_load (FILE *f, GError **error)
{
	return xcf_image_load_real (f, NULL, error);
}


/* Progressive loader */

/* 
 * as the layers are packed top down in the xcf format, and we have to render them bottom-up,
 * we need the full file loaded to start rendering
 */

static gpointer
xcf_image_begin_load (GdkPixbufModuleSizeFunc size_func,
		GdkPixbufModulePreparedFunc prepare_func,
		GdkPixbufModuleUpdatedFunc update_func,
		gpointer user_data,
		GError **error)
{
	LOG ("Begin\n");
	XcfContext *context;
	gint fd;

	context = g_new (XcfContext, 1);
	context->size_func = size_func;
	context->prepare_func = prepare_func;
	context->update_func = update_func;
	context->user_data = user_data;
	context->type = FILETYPE_UNKNOWN;
	context->bz_stream = NULL;

	fd = g_file_open_tmp ("gdkpixbuf-xcf-tmp.XXXXXX", &context->tempname, NULL);
	
	if (fd < 0) {
		g_free (context);
		return NULL;
	}

	context->file = fdopen (fd, "w+");
	if (!context->file) {
		g_free (context->tempname);
		g_free (context);
		return NULL;
	}

	return context;
}

static gboolean
xcf_image_stop_load (gpointer data, GError **error)
{
	LOG ("Stop\n");
	XcfContext *context = (XcfContext*) data;
	gboolean retval = TRUE;

	g_return_val_if_fail (data, TRUE);

	fflush (context->file);
	rewind (context->file);
	GdkPixbuf *pixbuf = xcf_image_load_real (context->file, context, error);
	if (!pixbuf)
		retval = FALSE;
	else
		g_object_unref (pixbuf);
	fclose (context->file);
	g_unlink (context->tempname);
	g_free (context->tempname);
	g_free (context);

	return retval;
}

static gboolean
xcf_image_load_increment (gpointer data,
		    const guchar *buf,
		    guint size,
		    GError **error)
{
	LOG ("Increment %d\n", size);
	g_return_val_if_fail (data, FALSE);
	XcfContext *context = (XcfContext*) data;

	if (context->type == FILETYPE_UNKNOWN) { // first chunk
		if (!strncmp (buf, "gimp xcf ", 9))
			context->type = FILETYPE_XCF;
		if (!strncmp (buf, "BZh", 3)) {
			context->type = FILETYPE_XCF_BZ2;

			//Initialize bzlib
			context->bz_stream = g_new (bz_stream, 1);
			context->bz_stream->bzalloc = NULL;
			context->bz_stream->bzfree = NULL;
			context->bz_stream->opaque = NULL;

			int ret = BZ2_bzDecompressInit (context->bz_stream, 4, 0); //Verbosity = 4, don't optimize for memory usage
			if (ret != BZ_OK) {
				g_set_error_literal (error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED, "Failed to initialize bz2 decompressor");
				return FALSE;
			}

		}
		LOG ("File type %d\n", context->type);
	}

	gchar *outbuf;
	switch (context->type) {
	case FILETYPE_XCF_BZ2:
		outbuf = g_new (gchar, 65536);
		context->bz_stream->next_in = (gchar*)buf;
		context->bz_stream->avail_in = size;
		while (context->bz_stream->avail_in > 0) {
			context->bz_stream->next_out = outbuf;
			context->bz_stream->avail_out = 65536;
			LOG ("BEFORE:\tnext_in %p, avail_in %d, total_in %d, next_out %p, avail_out %d total_out %d\n",
					context->bz_stream->next_in,
					context->bz_stream->avail_in,
					context->bz_stream->total_in_lo32,
					context->bz_stream->next_out,
					context->bz_stream->avail_out,
					context->bz_stream->total_out_lo32);
			int ret = BZ2_bzDecompress (context->bz_stream);
			LOG ("AFTER:\tnext_in %p, avail_in %d, total_in %d, next_out %p, avail_out %d total_out %d\n",
					context->bz_stream->next_in,
					context->bz_stream->avail_in,
					context->bz_stream->total_in_lo32,
					context->bz_stream->next_out,
					context->bz_stream->avail_out,
					context->bz_stream->total_out_lo32);
			switch (ret) {
			case BZ_OK:
				break;
			case BZ_STREAM_END:
				LOG ("End of bz stream\n");
				BZ2_bzDecompressEnd (context->bz_stream);
				//FIXME set the filetype to CLOSED
				break;
			default:
				BZ2_bzDecompressEnd (context->bz_stream);
				//FIXME set the filetype to CLOSED
				g_set_error_literal (error, GDK_PIXBUF_ERROR, GDK_PIXBUF_ERROR_FAILED, "Failed to decompress");
				return FALSE;
			}

			int total_out = 65536 - context->bz_stream->avail_out;
			LOG ("Wrote %d to file %s\n", total_out, context->tempname);
			if (fwrite (outbuf, sizeof (guchar), total_out, context->file) != total_out) {
				gint save_errno = errno;
				g_set_error_literal (error,
						     G_FILE_ERROR,
						     g_file_error_from_errno (save_errno),
						     "Failed to write to temporary file when loading Xcf image");
				return FALSE;
			}
		}
		break;
	case FILETYPE_XCF:
	default:
		if (fwrite (buf, sizeof (guchar), size, context->file) != size) {
			gint save_errno = errno;
			g_set_error_literal (error,
					     G_FILE_ERROR,
					     g_file_error_from_errno (save_errno),
					     "Failed to write to temporary file when loading Xcf image");
			return FALSE;
		}
		break;
	}

	return TRUE;	
}


#define MODULE_ENTRY(function) G_MODULE_EXPORT void function
MODULE_ENTRY (fill_vtable) (GdkPixbufModule *module)
{
        module->load = xcf_image_load;
        module->begin_load = xcf_image_begin_load;
        module->stop_load = xcf_image_stop_load;
        module->load_increment = xcf_image_load_increment;
}

MODULE_ENTRY (fill_info) (GdkPixbufFormat *info)
{
        static GdkPixbufModulePattern signature[] = {
                { "gimp xcf", NULL, 100 },
		{ "BZh", NULL, 80 },
                { NULL, NULL, 0 }
        };
	static gchar * mime_types[] = {
		"image/x-xcf",
		"image/x-compressed-xcf",
		NULL
	};
	static gchar * extensions[] = {
		"xcf",
		"xcf.bz2",
		NULL
	};

	info->name = "xcf";
        info->signature = signature;
	info->description = "The XCF (The Gimp) image format";
	info->mime_types = mime_types;
	info->extensions = extensions;
	info->flags = 0;
	info->license = "LGPL";
}
