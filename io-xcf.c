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
 * LICENCE BLAH
 *
 */
#define GDK_PIXBUF_ENABLE_BACKEND

#include <gmodule.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk-pixbuf/gdk-pixbuf-io.h>
#include <math.h>
#include <string.h>

#define LOG(...) printf (__VA_ARGS__);

//FIXME: change this macro to noop on bigendian machines
#define SWAP(int32) ( ((int32) >> 24) + \
		      ((int32) >> 8 & 0x0000FF00 )+ \
		      ((int32) << 8 & 0x00FF0000 )+ \
		      ((int32) << 24) )

#define PROP_END 		0
#define PROP_COLORMAP 		1
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

typedef struct _XcfChannel XcfChannel;
struct _XcfChannel {
	guint32 width;
	guint32 height;
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
		case 0: channels = 3; break;
		case 1: channels = 4; break;
		case 2: channels = 1; break;
		case 3: channels = 2; break;
		case 4: channels = 1; break;
		case 5: channels = 2; break;
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

	//re-interlace
	int i;

	for (i=0; i<count;i++)
		switch (type) {
		case 0:
			memcpy (ptr + 4*i + 0, ch[0] + i, 1);
			memcpy (ptr + 4*i + 1, ch[1] + i, 1);
			memcpy (ptr + 4*i + 2, ch[2] + i, 1);
			ptr[4*i + 3] = 0xff;
			break;
		case 1:
			memcpy (ptr + 4*i + 0, ch[0] + i, 1);
			memcpy (ptr + 4*i + 1, ch[1] + i, 1);
			memcpy (ptr + 4*i + 2, ch[2] + i, 1);
			memcpy (ptr + 4*i + 3, ch[3] + i, 1);
			break;
		case 2:
			memcpy (ptr + 4*i + 0, ch[0] + i, 1);
			memcpy (ptr + 4*i + 1, ch[0] + i, 1);
			memcpy (ptr + 4*i + 2, ch[0] + i, 1);	
			ptr[4*i + 3] = 0xff;
			break;
		case 3:
			memcpy (ptr + 4*i + 0, ch[0] + i, 1);
			memcpy (ptr + 4*i + 1, ch[0] + i, 1);
			memcpy (ptr + 4*i + 2, ch[0] + i, 1);	
			memcpy (ptr + 4*i + 3, ch[1] + i, 1);
			break;
		}
}

static GdkPixbuf*
xcf_image_load (FILE *f, GError **error)
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
	LOG ("%s\n", buffer);
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

	LOG ("W: %d, H: %d, mode: %d\n", width, height, color_mode);
		
	//Image Properties
	while (1) {
		fread (property, sizeof(guint32), 2, f); //read property and payload
		if (!property[0])
			break;
		property[0] = SWAP(property[0]);
		property[1] = SWAP(property[1]);
		LOG ("property %d, payload %d\n", property[0], property[1]);
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
		if (!layer)
			g_set_error_literal (error,
			     GDK_PIXBUF_ERROR,
			     GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY,
			     "Cannot allocate memory for loading XCF image");	

		layer->mode = 0;
		layer->apply_mask = FALSE;
		layer->dx = layer->dy = 0;
		layer->visible = TRUE;
		layer->opacity = 0xff;

		LOG ("layer_ptr: %d\n", layer_ptr);
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
			LOG ("\tproperty %d, payload %d\n", property[0], property[1]);
			gint32 offset[2];
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
				if (SWAP(data[0]) == 0)
					layer->visible = FALSE;
				break;
			case PROP_APPLY_MASK:
				fread (data, sizeof(guint32), 1, f);
				if (SWAP(data[0]) == 1)
					layer->apply_mask = TRUE;
				break;
			case PROP_OFFSETS:
				fread(offset, sizeof(gint32), 2, f);
				layer->dx = SWAP(offset[0]);
				layer->dy = SWAP(offset[1]);
				break;
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
		LOG ("\tHierarchy w:%d, h:%d, bpp:%d\n", data[0], data[1], data[2]);

		guint32 lptr;
		fread (&lptr, sizeof(guint32), 1, f);
		layer->lptr = SWAP (lptr);
//		long pos2 = ftell (f);
//		//jump to level
//		fseek (f, lptr, SEEK_SET);
//		//Ignore Level w and h (same as hierarchy)
//		fseek (f, 2 * sizeof(guint32), SEEK_CUR);
//		//Iterate on the tiles
//		guint32 tptr;
//		while (1) {
//			fread (&tptr, sizeof(guint32), 1, f);
//			if (!tptr)
//				break;
//			tptr = SWAP(tptr);
//			LOG("\tTile %d\n", tptr);
//			FIXME: compute tile size, rle decode each channel (if rle), re-interlace rgba
//		}
//
//		//rewind to the hierarchy position
//		fseek (f, pos2, SEEK_SET);

		//Here I could iterate over the unused dlevels and skip them

		//rewind to the layer position
		fseek (f, pos1, SEEK_SET);

		//Mask Pointer
		guint32 mptr;
		fread (&mptr, sizeof(guint32), 1, f);


		//rewind to the previous position
		fseek (f, pos, SEEK_SET);

		
		layers = g_list_prepend (layers, layer); //prepend so the layers are in a bottom-up order in the list
	}

	//Channels
	guint32 channel_ptr;
	while (1) {
		fread (&channel_ptr, sizeof(guint32), 1, f);
		if (!channel_ptr)
			break;

		channel_ptr = SWAP(channel_ptr);

		LOG ("channel_ptr: %d\n", channel_ptr);
		long pos = ftell (f);
		//jump to the channel
		fseek(f, channel_ptr, SEEK_SET);

		//Channel w, h
		fread (data, sizeof(guint32), 2, f);
		data[0] = SWAP(data[0]);
		data[1] = SWAP(data[1]);
		LOG ("\tChannel w:%d, h:%d\n", data[0], data[1]);

		//Channel name, ignore
		guint32 string_size;
		fread (&string_size, sizeof(guint32), 1, f);
		fseek (f, SWAP(string_size), SEEK_CUR);

		//Channel properties, skip them all
		while (1) {
			fread (property, sizeof(guint32), 2, f); //property and payload
			if (!property[0])
				break;		//break on PROP_END
			property[0] = SWAP (property[0]);
			property[1] = SWAP (property[1]);
			LOG ("\tproperty %d, payload %d\n", property[0], property[1]);
			switch (property[0]) {
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

		//rewind...
		fseek (f, pos1, SEEK_SET);


		//rewind to the previous position
		fseek (f, pos, SEEK_SET);

	}

	//Compose the pixbuf
	pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
	LOG ("pixbuf %d %d\n", gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf));
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

		fseek (f, layer->lptr, SEEK_SET);
		//Ignore Level w and h (same as hierarchy)
		fseek (f, 2 * sizeof(guint32), SEEK_CUR);
		//Iterate on the tiles
		guint32 tptr;
		int tile_id = 0;
		guchar *pixs = gdk_pixbuf_get_pixels (pixbuf);
		int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
		
		while (1) {
			fread (&tptr, sizeof(guint32), 1, f);
			if (!tptr)
				break;
			tptr = SWAP(tptr);
			long lpos = ftell (f);
			fseek (f, tptr, SEEK_SET);
			LOG("\tTile %d %d\n", tile_id, tptr);
			rle_decode (f, pixels, 64*64, layer->type);

			int tw = ceil ((layer->width) / 64);
			int origin = 64 * 4 * (tile_id % tw) + 64 * rowstride * (tile_id/tw) ;

			int j;
			for (j=0; j<64;j++) {
				memcpy (pixs + origin + j * rowstride, pixels + j*64*4 , 64*4);
			}


			fseek (f, lpos, SEEK_SET);
			tile_id++;
		}


		//only rneders bg for now
		break;
	}

	return pixbuf;
}

static gpointer
xcf_image_begin_load (GdkPixbufModuleSizeFunc size_func,
		GdkPixbufModulePreparedFunc prepare_func,
		GdkPixbufModuleUpdatedFunc update_func,
		gpointer user_data,
		GError **error)
{
	g_set_error_literal (error,
			     GDK_PIXBUF_ERROR,
			     GDK_PIXBUF_ERROR_UNSUPPORTED_OPERATION,
			     "Progressive loader not implemented(yet), use synchronous loader");
	return NULL;
}

static gboolean
xcf_image_stop_load (gpointer context, GError **error)

{
	g_set_error_literal (error,
			     GDK_PIXBUF_ERROR,
			     GDK_PIXBUF_ERROR_UNSUPPORTED_OPERATION,
			     "Progressive loader not implemented(yet), use synchronous loader");
	return FALSE;
}

static gboolean
xcf_image_load_increment (gpointer context,
		    const guchar *buf,
		    guint size,
		    GError **error)
{
	g_set_error_literal (error,
			     GDK_PIXBUF_ERROR,
			     GDK_PIXBUF_ERROR_UNSUPPORTED_OPERATION,
			     "Progressive loader not implemented(yet), use synchronous loader");
	return FALSE;	
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
                { NULL, NULL, 0 }
        };
	static gchar * mime_types[] = {
		"image/x-xcf",
		NULL
	};
	static gchar * extensions[] = {
		"xcf",
		NULL
	};

	info->name = "xcf";
        info->signature = signature;
	info->description = "The XCF (gimp) image format";
	info->mime_types = mime_types;
	info->extensions = extensions;
	info->flags = 0;
	info->license = "LGPL";
}
