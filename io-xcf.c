/*
 * Pixbuf loader for xcf
 *
 * Author(s):
 *	Stephane Delcroix  <stephane@delcroix.org>
 *
 * Copyright (C) 2009 Novell, Inc
 *
 * LICENCE BLAH
 */
#include "gdk-pixbuf.h"
#include "gdk-pixbuf-io.h"

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
	info->description = N_("The XCF (gimp) image format");
	info->mime_types = mime_types;
	info->extensions = extensions;
	info->flags = 0;
	info->license = "LGPL";
}
