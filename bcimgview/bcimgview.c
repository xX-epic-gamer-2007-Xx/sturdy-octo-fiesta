/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* Badly Coded Image Viewer BCImgView, a product of Badly Coded,
   Inc. */

/* This is an example insecure program for CSci 4271 only: don't copy
   code from here to any program that is supposed to work
   correctly! */

/* The GUI parts of this program are modeled after an example image
   viewer from an older version of the Gnome developer documentation,
   which has a similar looking interface but supports standard image
   formats without parsing them itself. It seems to not be currently
   hosted on the Gnome developer web page, but it can still be found
   in a few places online, like: */

/*
  https://tecnocode.co.uk/misc/platform-demos/image-viewer.c.xhtml
  https://web.archive.org/web/20210306070147/https://developer.gnome.org/gnome-devel-demos/stable/image-viewer.c.html.en
*/

/* Code that was copied verbatim from a non-buggy source is probably
   less likely to contain vulnerabilities. However, you shouldn't
   believe everything that comments in this code tell you; the
   comments might have bugs too.
 */

/* This disables some defense mechanisms. */
#undef _FORTIFY_SOURCE

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/time.h>
#include <sys/resource.h>

#ifndef DISABLE_GUI
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif

#include "bcimgview-core"

#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif


#ifndef DISABLE_GUI
/* Display an image by converting it from image_info format into the
   GDK Pixbuf structure, and associating that pixbuf with an image
   widget in the GUI. */
void display_image(struct image_info *info, GtkWidget *image) {
    GdkPixbuf *pixbuf;
    long rowstride, y;
    guchar *pixels;
    size_t rowsize;

    /* Make a pixmap with 8 bits per sample RGB and no alpha */
    pixbuf = 
        gdk_pixbuf_new(GDK_COLORSPACE_RGB, 0, 8, info->width, info->height);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    pixels = gdk_pixbuf_get_pixels(pixbuf);
    g_assert(gdk_pixbuf_get_n_channels(pixbuf) == 3);
    rowsize = 3 * info->width;

    /* The GDK pixbuf format is similar to ours, but sometimes has
       additional alignment padding between the rows. In other words,
       we have to copy row by row because GDK Pixbuf's rowstride might
       be bigger than our rowsize. */
    for (y = 0; y < info->height; y++) {
        memcpy(pixels + y * rowstride, info->pixels + y * rowsize, rowsize);
    }

    /* This is the function that passes the pixbuf to GTK. */
    gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);

    print_log_msg(info);

    /* After copying to the pixbuf, the image_info isn't needed
       anymore. */
    free_image_info(info);
    (*per_image_callback)();
}
#endif

/* Write an internal-formatted image into a file in the common Unix
   PPM format. Conveniently, the body of the PPM format is the same as
   our internal pixel format, only a different header is needed. */
void write_ppm(struct image_info *info, const char *out_fname) {
    FILE *fh = fopen(out_fname, "wb");
    int res;
    size_t num_written;
    if (!fh) {
        fprintf(stderr, "Failed to open %s for writing: %s\n",
                out_fname, strerror(errno));
        exit(1);
    }
    /* 255 is called the "maxval" in PPM terminolgy, and corresponds
       to 8 bits per sample. */
    fprintf(fh, "P6\n%ld %ld\n255\n", info->width, info->height);
    num_written = fwrite(info->pixels, 3 * info->width, info->height, fh);
    if (num_written != info->height) {
        fprintf(stderr, "Unable to write complete image\n");
    }
    res = fclose(fh);
    if (res != 0) {
        fprintf(stderr, "Failure on closing output: %s\n", strerror(errno));
    }
}

#ifndef DISABLE_GUI
/* Use a GTK file chooser to let a user graphically select another
   image to display. */
static void on_open_image(GtkButton* button, gpointer user_data) {
    GtkWidget *image = GTK_WIDGET(user_data);
    GtkWidget *toplevel = gtk_widget_get_toplevel(image);
    GtkFileFilter *filter = gtk_file_filter_new();
    GtkWidget *dialog =
        gtk_file_chooser_dialog_new("Open image",
                                    GTK_WINDOW(toplevel),
                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                    "_Open", GTK_RESPONSE_ACCEPT,
                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                    NULL);

    /* The chooser will only display files with extensions
       corresponding to our supported formats. */
    gtk_file_filter_add_pattern(filter, "*.bcraw");
    gtk_file_filter_add_pattern(filter, "*.bcprog");
    gtk_file_filter_add_pattern(filter, "*.bcflat");
    gtk_file_filter_set_name(filter, "Badly-coded format images");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER (dialog),
                                filter);
        
    switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
    case GTK_RESPONSE_ACCEPT:
        {
            gchar *filename = 
                gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            struct image_info *info = parse_image(filename);
            if (info)
                display_image(info, image);
            break;
        }
    default:
        break;
    }
    gtk_widget_destroy(dialog);
}

/* This is the GTK image widget used for displaying images. */
GtkWidget *global_image;

/* Create the main GUI of the image viewer. */
static GtkWidget* create_window(void) {
    GtkWidget *window;
    GtkWidget *open_button;
    GtkWidget *image;
    GtkWidget *box;

    /* Set up the UI */
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "bcimgview");

    /* For our simple uses, GTK 2 and GTK 3 are almost completely
       compatible. This is the one line that needs to be different. */
#if GTK_MAJOR_VERSION >= 3
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
#else
    box = gtk_vbox_new(FALSE, 5);
#endif
    open_button = gtk_button_new_with_label("Open a different image");
    image = gtk_image_new();

    gtk_box_pack_start(GTK_BOX(box), image, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), open_button, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(window), box);

    /* Connect signals */

    /* Show open dialog when opening a file */
    g_signal_connect(open_button, "clicked", G_CALLBACK(on_open_image), image);
        
    /* Exit when the window is closed */
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    global_image = image;

    return window;
}
#endif

int main(int argc, char *argv[]) {
    int res;
    struct rlimit rlim;

    per_image_callback = &benign_target;

    res = getrlimit(RLIMIT_STACK, &rlim);
    if (res == 0 && rlim.rlim_cur != RLIM_INFINITY) {
        /* We store image data on the heap rather than the stack, but
           we use the size limit for the stack, if set, to estimate
           what would constitue a reasonable amount of memory to use
           on the system. */
        size_limit = (long)sqrt(rlim.rlim_cur * 1024 / 3);
    }

    if (argc == 3 && argv[1][0] == '-' && argv[1][1] == 'c' && !argv[1][2]) {
        /* Batch conversion mode; don't start the GUI. */
        struct image_info *info = parse_image(argv[2]);
        char *out_fname = xmalloc(strlen(argv[2]) + 5);
        if (!info)
            return 1;
        strcpy(out_fname, argv[2]);
        strcat(out_fname, ".ppm");
        printf("Batch conversion output in %s\n", out_fname);
        write_ppm(info, out_fname);
        free(out_fname);
        print_log_msg(info);
        free_image_info(info);
        (*per_image_callback)();
        return 0;
#ifdef DISABLE_GUI
    } else {
        fprintf(stderr, "Usage: bcimgview-nogui -c <image>\n");
        return 1;
    }
#else
    } else if (argc == 1 || argc == 2) {
        /* GUI mode */
        GtkWidget *window;
        gtk_init(&argc, &argv);

        window = create_window();
        gtk_widget_show_all(window);

        if (argc == 2) {
            struct image_info *info = parse_image(argv[1]);
            if (info)
                display_image(info, global_image);
        }

        gtk_main();
    } else {
        fprintf(stderr, "Usage: bcimgview [-c] [<image>]\n");
        return 1;
    }
#endif
    return 0;
}