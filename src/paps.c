/* Pango
 * paps.c: A postscript printing program using pango.
 *
 * Copyright (C) 2002, 2005 Dov Grobgeld <dov.grobgeld@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */


#include <pango/pango.h>
#include <pango/pangoft2.h>
#include <pango/pangocairo.h>
#include <cairo/cairo.h>
#include <cairo/cairo-ps.h>
#include <cairo/cairo-pdf.h>
#include <cairo/cairo-svg.h>
#ifdef HAVE_CUPS
#include <cups/cups.h>
#include <cups/ppd.h>
#endif
#include <errno.h>
#include <langinfo.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <math.h>
#include <wchar.h>

#define BUFSIZE 1024
#define DEFAULT_FONT_FAMILY	"Monospace"
#define DEFAULT_FONT_SIZE	"12"
#define HEADER_FONT_FAMILY	"Monospace Bold"
#define HEADER_FONT_SCALE	"12"
#define MAKE_FONT_NAME(f,s)	f " " s

typedef enum {
    PAPER_TYPE_A4 = 0,
    PAPER_TYPE_US_LETTER = 1,
    PAPER_TYPE_US_LEGAL = 2,
    PAPER_TYPE_A3 = 3
} paper_type_t ;

typedef enum {
    FORMAT_POSTSCRIPT = 0,
    FORMAT_PDF = 1,
    FORMAT_SVG = 2
} output_format_t ;

typedef struct  {
    double width;
    double height;
} paper_size_t;

const paper_size_t paper_sizes[] = {
    { 595.28, 841.89}, /* A4 */
    { 612, 792},       /* US letter */
    { 612, 1008},      /* US legal */
    { 842, 1190}       /* A3 */
};

typedef struct {
  int column_width;
  int column_height;
  int num_columns;
  int gutter_width;  /* These are all in postscript points=1/72 inch... */
  int top_margin;  
  int bottom_margin;
  int left_margin;
  int right_margin;
  double page_width;
  double page_height;
  int header_ypos;
  int header_sep;
  int header_height;
  int footer_height;
  gdouble scale_x;
  gdouble scale_y;
  gboolean do_draw_header;
  gboolean do_draw_footer;
  gboolean do_duplex;
  gboolean do_tumble;
  gboolean do_landscape;
  gboolean do_justify;
  gboolean do_separation_line;
  gboolean do_draw_contour;
  gboolean do_wordwrap;
  gboolean do_use_markup;
  gboolean do_stretch_chars;
  PangoDirection pango_dir;
  const gchar *filename;
  const gchar *header_font_desc;
  gdouble lpi;
  gdouble cpi;
#ifdef HAVE_CUPS
  gboolean cups_mode;
#endif
} page_layout_t;

typedef struct {
  char *text;
  int length;
} para_t;

typedef struct {
  PangoLayoutLine *pango_line;
  PangoRectangle logical_rect;
  PangoRectangle ink_rect;
  int formfeed;
} LineLink;

typedef struct _Paragraph Paragraph;

/* Structure representing a paragraph
 */
struct _Paragraph {
  const char *text;
  int length;
  int height;   /* Height, in pixels */
  int formfeed;
  PangoLayout *layout;
};

/* Information passed in user data when drawing outlines */
GList        *split_paragraphs_into_lines  (page_layout_t   *page_layout,
					    GList           *paragraphs);
static char  *read_file                    (FILE            *file,
                                            GIConv           handle);
static GList *split_text_into_paragraphs   (cairo_t *cr,
                                            PangoContext    *pango_context,
                                            page_layout_t   *page_layout,
                                            int              paint_width,
                                            const char      *text);
static int    output_pages                 (cairo_surface_t * surface,
                                            cairo_t         *cr,
                                            GList           *pango_lines,
                                            page_layout_t   *page_layout,
                                            gboolean         need_header,
                                            PangoContext    *pango_context);
static void   eject_column                 (cairo_t         *cr,
                                            page_layout_t   *page_layout,
                                            int              column_idx);
static void   eject_page                   (cairo_t         *cr);
static void   start_page                   (cairo_surface_t *surface,
                                            cairo_t         *cr, 
                                            page_layout_t   *page_layout,
                                            int              page_idx);
static void   draw_line_to_page            (cairo_t         *cr,
                                            int              column_idx,
                                            int              column_pos,
                                            page_layout_t   *page_layout,
                                            PangoLayoutLine *line);
static int    draw_page_header_line_to_page(cairo_t         *cr,
                                            gboolean         is_footer,
                                            page_layout_t   *page_layout,
                                            PangoContext    *ctx,
                                            int              page);

int last_char_idx = 0;
double last_pos_y = -1;
double last_pos_x = -1;
FILE *output_fh;
paper_type_t paper_type = PAPER_TYPE_A4;
output_format_t output_format = FORMAT_POSTSCRIPT;
PangoGravity gravity = PANGO_GRAVITY_AUTO;
PangoGravityHint gravity_hint = PANGO_GRAVITY_HINT_NATURAL;
const char *opt_language = NULL;

#define CASE(s) if (strcmp(S_, s) == 0)

static gboolean
_paps_arg_paper_cb(const char *option_name,
                   const char *value,
                   gpointer    data)
{
  gboolean retval = TRUE;
  
  if (value && *value)
    {
      if (g_ascii_strcasecmp(value, "legal") == 0)
        paper_type = PAPER_TYPE_US_LEGAL;
      else if (g_ascii_strcasecmp(value, "letter") == 0)
        paper_type = PAPER_TYPE_US_LETTER;
      else if (g_ascii_strcasecmp(value, "a4") == 0)
        paper_type = PAPER_TYPE_A4;
      else if (g_ascii_strcasecmp(value, "a3") == 0)
        paper_type = PAPER_TYPE_A3;
      else {
        retval = FALSE;
        fprintf(stderr, "Unknown page size name: %s.\n", value);
      }
    }
  else
    {
      fprintf(stderr, "You must specify page size.\n");
      retval = FALSE;
    }
  
  return retval;
}

static gboolean
_paps_arg_gravity_cb(const char *option_name,
                     const char *value,
                     gpointer    data)
{
  gboolean retval = TRUE;
  
  if (value && *value)
    {
      if (g_ascii_strcasecmp(value, "south") == 0)
        gravity = PANGO_GRAVITY_SOUTH;
      else if (g_ascii_strcasecmp(value, "east") == 0)
        gravity = PANGO_GRAVITY_EAST;
      else if (g_ascii_strcasecmp(value, "north") == 0)
        gravity = PANGO_GRAVITY_NORTH;
      else if (g_ascii_strcasecmp(value, "west") == 0)
        gravity = PANGO_GRAVITY_WEST;
      else if (g_ascii_strcasecmp(value, "auto") == 0)
        gravity = PANGO_GRAVITY_AUTO;
      else {
        retval = FALSE;
        fprintf(stderr, "Unknown gravity name: %s.\n", value);
      }
    }
  else
    {
      fprintf(stderr, "You must specify gravity.\n");
      retval = FALSE;
    }
  
  return retval;
}

static gboolean
_paps_arg_gravity_hint_cb(const char *option_name,
                          const char *value,
                          gpointer    data)
{
  gboolean retval = TRUE;
  
  if (value && *value)
    {
      if (g_ascii_strcasecmp(value, "neutral") == 0)
        gravity_hint = PANGO_GRAVITY_HINT_NATURAL;
      else if (g_ascii_strcasecmp(value, "strong") == 0)
        gravity_hint = PANGO_GRAVITY_HINT_STRONG;
      else if (g_ascii_strcasecmp(value, "line") == 0)
        gravity_hint = PANGO_GRAVITY_HINT_LINE;
      else {
        retval = FALSE;
        fprintf(stderr, "Unknown gravity hint name: %s.\n", value);
      }
    }
  else
    {
      fprintf(stderr, "You must specify gravity hint.\n");
      retval = FALSE;
    }
  
  return retval;
}

static gboolean
_paps_arg_format_cb(const char *option_name,
                    const char *value,
                    gpointer    data)
{
  gboolean retval = TRUE;
  
  if (value && *value)
    {
      if (g_ascii_strcasecmp(value, "pdf") == 0)
        output_format = FORMAT_PDF;
      else if (g_ascii_strcasecmp(value, "ps") == 0
               || g_ascii_strcasecmp(value, "postscript") == 0)
        output_format = FORMAT_POSTSCRIPT;
      else if (g_ascii_strcasecmp(value, "svg") == 0)
        output_format = FORMAT_SVG;
      else {
        retval = FALSE;
        fprintf(stderr, "Unknown output format: %s.\n", value);
      }
    }
  else
    {
      fprintf(stderr, "You must specify a output format.\n");
      retval = FALSE;
    }
  
  return retval;
}

static gboolean
_paps_arg_lpi_cb(const gchar *option_name,
		 const gchar *value,
		 gpointer     data)
{
  gboolean retval = TRUE;
  gchar *p = NULL;
  page_layout_t *page_layout = (page_layout_t*)data;

  if (value && *value)
    {
      errno = 0;
      page_layout->lpi = g_strtod(value, &p);
      if ((p && *p) || errno == ERANGE)
        {
          fprintf(stderr, "given LPI value was invalid.\n");
          retval = FALSE;
        }
    }
  else
    {
      fprintf(stderr, "You must specify the amount of lines per inch.\n");
      retval = FALSE;
    }

  return retval;
}

static gboolean
_paps_arg_cpi_cb(const gchar *option_name,
		 const gchar *value,
		 gpointer     data)
{
  gboolean retval = TRUE;
  gchar *p = NULL;
  page_layout_t *page_layout = (page_layout_t*)data;
  
  if (value && *value)
    {
      errno = 0;
      page_layout->cpi = g_strtod(value, &p);
      if ((p && *p) || errno == ERANGE)
        {
          fprintf(stderr, "given CPI value was invalid.\n");
          retval = FALSE;
        }
    }
  else
    {
      fprintf(stderr, "You must specify the amount of characters per inch.\n");
      retval = FALSE;
    }

  return retval;
}

static PangoLanguage *
get_language(void)
{
  PangoLanguage *retval;
  gchar *lang = g_strdup (setlocale (LC_CTYPE, NULL));
  gchar *p;

  p = strchr (lang, '.');
  if (p)
    *p = 0;
  p = strchr (lang, '@');
  if (p)
    *p = 0;

  retval = pango_language_from_string (lang);
  g_free (lang);

  return retval;
}

static cairo_status_t paps_cairo_write_func(void *closure,
                                            const unsigned char *data,
                                            unsigned int length)
{
  fwrite(data,length,1,output_fh);
  return CAIRO_STATUS_SUCCESS;
}

int main(int argc, char *argv[])
{
  gboolean do_landscape = FALSE, do_rtl = FALSE, do_justify = FALSE, do_draw_header = FALSE;
  gboolean do_stretch_chars = FALSE;
  gboolean do_use_markup = FALSE;
  gboolean do_encoding_from_lang = FALSE;
  gboolean do_wordwrap = TRUE; // What should be default?
  int num_columns = 1;
  int top_margin = 36, bottom_margin = 36, right_margin = 36, left_margin = 36;
  gboolean do_fatal_warnings = FALSE;
  const gchar *font = MAKE_FONT_NAME (DEFAULT_FONT_FAMILY, DEFAULT_FONT_SIZE);
  gchar *encoding = NULL;
  gchar *output = NULL;
  page_layout_t page_layout;
  GOptionContext *ctxt = g_option_context_new("[text file]");
  GOptionEntry entries[] = {
    {"landscape", 0, 0, G_OPTION_ARG_NONE, &do_landscape,
     "Landscape output. (Default: portrait)", NULL},
    {"stretch-chars", 0, 0, G_OPTION_ARG_NONE, &do_stretch_chars,
     "Whether to stretch characters in y-direction to fill lines. (Default: no)", NULL},
    {"markup", 0, 0, G_OPTION_ARG_NONE, &do_use_markup,
     "Should the text be considered pango markup? (Default: no)", NULL},
    {"columns", 0, 0, G_OPTION_ARG_INT, &num_columns,
     "Number of columns output. (Default: 1)", "NUM"},
    {"font", 0, 0, G_OPTION_ARG_STRING, &font,
     "Set the font description. (Default: Monospace 12)", "DESC"},
    {"output", 'o', 0, G_OPTION_ARG_STRING, &output,
     "Output file. (Default stdout)", "DESC"},
    {"rtl", 0, 0, G_OPTION_ARG_NONE, &do_rtl,
     "Do rtl layout.", NULL},
    {"justify", 0, 0, G_OPTION_ARG_NONE, &do_justify,
     "Justify the layout.", NULL},
    {"paper", 0, 0, G_OPTION_ARG_CALLBACK, _paps_arg_paper_cb,
     "Choose paper size. Known paper sizes are legal,\n"
     "letter, a3, a4. (Default: a4)", "PAPER"},
    {"gravity", 0, 0, G_OPTION_ARG_CALLBACK, _paps_arg_gravity_cb,
     "Base gravity: glyph rotation. Defaut: auto", "GRAVITY"},
    {"gravity-hint", 0, 0, G_OPTION_ARG_CALLBACK, _paps_arg_gravity_hint_cb,
     "Gravity hint", "HINT"},
    {"format", 0, 0, G_OPTION_ARG_CALLBACK, _paps_arg_format_cb,
     "Choose output format. Known formats are pdf, svg, ps. (Default ps)\n"},
    {"language", 0, 0, G_OPTION_ARG_STRING, &opt_language,
     "Language to use for font selection", "en_US/etc"},
    {"bottom-margin", 0, 0, G_OPTION_ARG_INT, &bottom_margin,
     "Set bottom margin in postscript point units (1/72inch). (Default: 36)", "NUM"},
    {"top-margin", 0, 0, G_OPTION_ARG_INT, &top_margin,
     "Set top margin. (Default: 36)", "NUM"},
    {"right-margin", 0, 0, G_OPTION_ARG_INT, &right_margin,
     "Set right margin. (Default: 36)", "NUM"},
    {"left-margin", 0, 0, G_OPTION_ARG_INT, &left_margin,
     "Set left margin. (Default: 36)", "NUM"},
    {"header", 0, 0, G_OPTION_ARG_NONE, &do_draw_header,
     "Draw page header for each page.", NULL},
    {"encoding", 0, 0, G_OPTION_ARG_STRING, &encoding,
     "Assume the documentation encoding.", "ENCODING"},
    {"lang-encoding", 0, 0, G_OPTION_ARG_NONE, &do_encoding_from_lang,
     "Determine the encoding from the language setting. (Default: no)", NULL},
    {"lpi", 0, 0, G_OPTION_ARG_CALLBACK, _paps_arg_lpi_cb,
     "Set the amount of lines per inch.", "REAL"},
    {"cpi", 0, 0, G_OPTION_ARG_CALLBACK, _paps_arg_cpi_cb,
     "Set the amount of characters per inch.", "REAL"},
    {"g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &do_fatal_warnings,
     "Set glib fatal warnings", "REAL"},
    
    {NULL}

  };
  GError *error = NULL;
  FILE *IN = NULL;
  GList *paragraphs;
  GList *pango_lines;
  PangoContext *pango_context;
  PangoFontDescription *font_description;
  PangoDirection pango_dir = PANGO_DIRECTION_LTR;
  PangoFontMap *fontmap;
  PangoFontset *fontset;
  PangoFontMetrics *metrics;
  int gutter_width = 40;
  int total_gutter_width;
  double page_width = -1;
  double page_height = -1;
  int do_tumble = -1;   /* -1 means not initialized */
  int do_duplex = -1;
  const gchar *header_font_desc = MAKE_FONT_NAME (HEADER_FONT_FAMILY, HEADER_FONT_SCALE);
  const gchar *filename_in;
  gchar *text;
  int header_sep = 20;
  int max_width = 0, w;
  GIConv cvh = NULL;
  GOptionGroup *options;
  cairo_t *cr;
  cairo_surface_t *surface = NULL;
  double surface_page_width = 0, surface_page_height = 0;
#ifdef HAVE_CUPS
  gboolean cups_mode = FALSE;
  gchar *page_owner = NULL, *title = NULL;
#endif

  /* Set locale from environment */
  setlocale(LC_ALL, "");
  
  /* Init page_layout_t parameters set by the option parsing */
  page_layout.cpi = page_layout.lpi = 0;

  options = g_option_group_new("main","","",&page_layout, NULL);
  g_option_group_add_entries(options, entries);
  g_option_context_set_main_group(ctxt, options);
#if 0
  g_option_context_add_main_entries(ctxt, entries, NULL);
#endif

#ifdef HAVE_CUPS
  /* check if the process is being invoked as CUPS filter */
  G_STMT_START
    {
      gchar *prgname = g_path_get_basename (argv[0]);
      cups_option_t *cups_options = NULL;
      ppd_file_t *ppd;
      ppd_size_t *pagesize;
      int num_options;
      const char *val;

      if (strncmp(prgname, "texttopaps", 10) == 0 ||
	  getenv("CUPS_SERVER") != NULL)
	{
	  g_set_prgname(prgname);
	  /* argument format should be job-id user title copies options [file] */
	  cups_mode = TRUE;
	  /* set default values */
	  page_layout.lpi = 6.0L;
	  page_layout.cpi = 10.0L;
	  left_margin = 36;
	  right_margin = 36;
	  top_margin = 36;
	  bottom_margin = 36;
	  page_width = 612;
	  page_height = 792;
	  font = g_strdup(MAKE_FONT_NAME ("Courier", DEFAULT_FONT_SIZE));
	  header_font_desc = g_strdup(MAKE_FONT_NAME ("Courier", HEADER_FONT_SCALE));
	  do_stretch_chars = TRUE;

	  if (argc < 6 || argc > 7)
	    {
	      fprintf(stderr, "ERROR: %s job-id user title copies options [file]\n", prgname);
	      exit(1);
	    }
	  if (argc == 6)
	    {
	      filename_in = "stdin";
	      IN = stdin;
	    }
	  else
	    {
	      filename_in = argv[6];
	      if ((IN = fopen(filename_in, "rb")) == NULL)
		{
		  fprintf(stderr, "ERROR: unable to open print file -\n");
		  exit(1);
		}
	    }
	  title = argv[3];
	  page_owner = argv[2];
	  num_options = cupsParseOptions(argv[5], 0, &cups_options);

	  if ((val = cupsGetOption("prettyprint", num_options, cups_options)) != NULL &&
	      g_ascii_strcasecmp(val, "no") &&
	      g_ascii_strcasecmp(val, "off") &&
	      g_ascii_strcasecmp(val, "false"))
	    {
	      /* XXX: need to support the keywords highlighting */
	    }
	  ppd = ppdOpenFile(getenv("PPD"));
	  ppdMarkDefaults(ppd);
	  cupsMarkOptions(ppd, num_options, cups_options);

	  if ((pagesize = ppdPageSize(ppd, NULL)) != NULL)
	    {
	      page_width = pagesize->width;
	      page_height = pagesize->length;
	      top_margin = pagesize->length - pagesize->top;
	      bottom_margin = pagesize->bottom;
	      left_margin = pagesize->left;
	      right_margin = pagesize->width - pagesize->right;
	    }

	  if ((val = cupsGetOption("landscape", num_options, cups_options)) != NULL)
	    {
	      if (g_ascii_strcasecmp(val, "no") &&
		  g_ascii_strcasecmp(val, "off") &&
		  g_ascii_strcasecmp(val, "false"))
		do_landscape = TRUE;
	    }
	  /* XXX: need to support orientation-requested? */
	  if ((val = cupsGetOption("page-left", num_options, cups_options)) != NULL)
	    {
	      left_margin = (int)atof(val);
	    }
	  if ((val = cupsGetOption("page-right", num_options, cups_options)) != NULL)
	    {
	      right_margin = (int)atof(val);
	    }
	  if ((val = cupsGetOption("page-bottom", num_options, cups_options)) != NULL)
	    {
	      bottom_margin = (int)atof(val);
	    }
	  if ((val = cupsGetOption("page-top", num_options, cups_options)) != NULL)
	    {
	      top_margin = (int)atof(val);
	    }
	  if (ppdIsMarked(ppd, "Duplex", "DuplexNoTumble") ||
	      ppdIsMarked(ppd, "Duplex", "DuplexTumble") ||
	      ppdIsMarked(ppd, "JCLDuplex", "DuplexNoTumble") ||
	      ppdIsMarked(ppd, "JCLDuplex", "DuplexTumble") ||
	      ppdIsMarked(ppd, "EFDuplex", "DuplexNoTumble") ||
	      ppdIsMarked(ppd, "EFDuplex", "DuplexTumble") ||
	      ppdIsMarked(ppd, "KD03Duplex", "DuplexNoTumble") ||
	      ppdIsMarked(ppd, "KD03Duplex", "DuplexTumble"))
	    {
	      do_duplex = TRUE;
	    }
	  if ((val = cupsGetOption("wrap", num_options, cups_options)) != NULL)
	    {
	      do_wordwrap = !g_ascii_strcasecmp(val, "true") ||
		!g_ascii_strcasecmp(val, "on") ||
		!g_ascii_strcasecmp(val, "yes");
	    }
	  if ((val = cupsGetOption("columns", num_options, cups_options)) != NULL)
	    {
	      num_columns = atoi(val);
	    }
	  if ((val = cupsGetOption("cpi", num_options, cups_options)) != NULL)
	    {
	      page_layout.cpi = atof(val);
	    }
	  if ((val = cupsGetOption("lpi", num_options, cups_options)) != NULL)
	    {
	      page_layout.lpi = atof(val);
	    }
	  if (getenv("CHARSET") != NULL)
	    {
	      const char *charset = getenv("CHARSET");
	      /* Map CUPS charset names to real ones.
	       * http://cups.org/newsgroups.php?s9797+gcups.general+v9797+T1
	       */
	      if (!g_ascii_strcasecmp(charset, "windows-932"))
		{
		  charset = "WINDOWS-31J";
		}
	      if (g_ascii_strcasecmp(charset, "utf-8") &&
		  g_ascii_strcasecmp(charset, "utf8"))
		{
		  encoding = g_strdup(charset);
		}
	    }
	}
    }
  G_STMT_END;

  if (!cups_mode)
    {
#endif

  /* Parse command line */
  if (!g_option_context_parse(ctxt, &argc, &argv, &error))
    {
      fprintf(stderr, "Command line error: %s\n", error->message);
      exit(1);
    }
  
  if (do_fatal_warnings)
    g_log_set_always_fatal(G_LOG_LEVEL_MASK);

  if (do_rtl)
    pango_dir = PANGO_DIRECTION_RTL;
  
  if (argc > 1)
    {
      filename_in = argv[1];
      IN = fopen(filename_in, "r");
      if (!IN)
        {
          fprintf(stderr, "Failed to open %s!\n", filename_in);
          exit(1);
        }
    }
  else
    {
      filename_in = "stdin";
      IN = stdin;
    }

#ifdef HAVE_CUPS
    }
#endif

  // For now always write to stdout
  if (output == NULL)
    output_fh = stdout;
  else
    {
      output_fh = fopen(output,"wb");
      if (!output_fh)
        {
          fprintf(stderr, "Failed to open %s for writing!\n", output);
          exit(1);
        }
    }

  /* Page layout */
  if (page_width < 0)
    page_width = paper_sizes[(int)paper_type].width;
  if (page_height < 0)
    page_height = paper_sizes[(int)paper_type].height;
  
  /* Swap width and height for landscape except for postscript */
  surface_page_width = page_width;
  surface_page_height = page_height;
  if (output_format != FORMAT_POSTSCRIPT && do_landscape)
    {
      surface_page_width = page_height;
      surface_page_height = page_width;
    }
        
  if (output_format == FORMAT_POSTSCRIPT)
    {
      surface = cairo_ps_surface_create_for_stream(&paps_cairo_write_func,
						   NULL,
						   surface_page_width,
						   surface_page_height);
#ifdef HAVE_CUPS
      G_STMT_START
	{
	  gchar *dsc;

	  dsc = g_strdup_printf ("%%%%For: %s", page_owner);
	  cairo_ps_surface_dsc_comment (surface, dsc);
	  g_free (dsc);
	  dsc = g_strdup_printf ("%%%%Title: %s", title);
	  cairo_ps_surface_dsc_comment (surface, dsc);
	  g_free (dsc);
	  /* Put %%cupsRotation tag to prevent the rotation in pstops.
	   * This breaks paps's behavior to make it in landscape say.
	   * (rhbz#222137)
	   */
	  cairo_ps_surface_dsc_comment (surface, "%%cupsRotation: 0");
	}
      G_STMT_END;
#endif
    }
  else if (output_format == FORMAT_PDF)
    surface = cairo_pdf_surface_create_for_stream(&paps_cairo_write_func,
                                                  NULL,
                                                  surface_page_width,
                                                  surface_page_height);
  else 
    surface = cairo_svg_surface_create_for_stream(&paps_cairo_write_func,
                                                  NULL,
                                                  surface_page_width,
                                                  surface_page_height);

  cr = cairo_create(surface);
  
  pango_context = pango_cairo_create_context(cr);
  pango_cairo_context_set_resolution(pango_context, 72.0); /* Native postscript resolution */
  
  /* Setup pango */
  pango_context_set_language (pango_context, get_language ());
  pango_context_set_base_dir (pango_context, pango_dir);
  pango_context_set_language (pango_context,
			      opt_language
                              ? pango_language_from_string (opt_language)
                              : pango_language_get_default ());
  pango_context_set_base_gravity (pango_context, gravity);
  pango_context_set_gravity_hint (pango_context, gravity_hint);
  
  /* create the font description */
  font_description = pango_font_description_from_string (font);
  if ((pango_font_description_get_set_fields (font_description) & PANGO_FONT_MASK_FAMILY) == 0)
    pango_font_description_set_family (font_description, DEFAULT_FONT_FAMILY);
  if ((pango_font_description_get_set_fields (font_description) & PANGO_FONT_MASK_SIZE) == 0)
    pango_font_description_set_size (font_description, atoi(DEFAULT_FONT_SIZE) * PANGO_SCALE);

  pango_context_set_font_description (pango_context, font_description);

  if (num_columns == 1)
    total_gutter_width = 0;
  else
    total_gutter_width = gutter_width * (num_columns - 1);
  if (do_landscape)
    {
      int tmp;
      tmp = page_width;
      page_width = page_height;
      page_height = tmp;
      if (do_tumble < 0)
        do_tumble = TRUE;
      if (do_duplex < 0)
        do_duplex = TRUE;
    }
  else
    {
      if (do_tumble < 0)
        do_tumble = TRUE;
      if (do_duplex < 0)
        do_duplex = TRUE;
    }
  
  page_layout.page_width = page_width;
  page_layout.page_height = page_height;
  page_layout.num_columns = num_columns;
  page_layout.left_margin = left_margin;
  page_layout.right_margin = right_margin;
  page_layout.gutter_width = gutter_width;
  page_layout.top_margin = top_margin;
  page_layout.bottom_margin = bottom_margin;
  page_layout.header_ypos = page_layout.top_margin;
  page_layout.header_height = 0;
  page_layout.footer_height = 0;
  page_layout.do_wordwrap = do_wordwrap;
  page_layout.scale_x = 1.0L;
  page_layout.scale_y = 1.0L;
  if (do_draw_header)
    page_layout.header_sep =  header_sep;
  else
    page_layout.header_sep = 0;
    
  page_layout.column_height = page_height
                            - page_layout.top_margin
                            - page_layout.header_sep
                            - page_layout.bottom_margin;
  page_layout.column_width =  (page_layout.page_width 
                            - page_layout.left_margin - page_layout.right_margin
                            - total_gutter_width) / page_layout.num_columns;
  page_layout.do_separation_line = TRUE;
  page_layout.do_landscape = do_landscape;
  page_layout.do_justify = do_justify;
  page_layout.do_stretch_chars = do_stretch_chars;
  page_layout.do_use_markup = do_use_markup;
  page_layout.do_tumble = do_tumble;
  page_layout.do_duplex = do_duplex;
  page_layout.pango_dir = pango_dir;
  page_layout.filename = filename_in;
  page_layout.header_font_desc = header_font_desc;
#ifdef HAVE_CUPS
  page_layout.cups_mode = cups_mode;
#endif

  /* calculate x-coordinate scale */
  if (page_layout.cpi > 0.0L)
    {
      gint font_size;

      fontmap = pango_ft2_font_map_new ();
      fontset = pango_font_map_load_fontset (fontmap, pango_context, font_description, get_language ());
      metrics = pango_fontset_get_metrics (fontset);
      max_width = pango_font_metrics_get_approximate_char_width (metrics);
      w = pango_font_metrics_get_approximate_digit_width (metrics);
      if (w > max_width)
	  max_width = w;
      page_layout.scale_x = 1 / page_layout.cpi * 72.0 * (gdouble)PANGO_SCALE / (gdouble)max_width;
      pango_font_metrics_unref (metrics);
      g_object_unref (G_OBJECT (fontmap));

      font_size = pango_font_description_get_size (font_description);
      // update the font size to that width
      pango_font_description_set_size (font_description, font_size * page_layout.scale_x);
      pango_context_set_font_description (pango_context, font_description);
    }

  page_layout.scale_x = page_layout.scale_y = 1.0;
      
  if (do_encoding_from_lang && encoding == NULL)
    {
      encoding = g_strdup(nl_langinfo(CODESET));
      if (!strcmp(encoding, "UTF-8"))
	{
	  g_free(encoding);
	  encoding = NULL;
	}
    }

  if (encoding != NULL)
    {
      cvh = g_iconv_open ("UTF-8", encoding);
      if (cvh == NULL)
        {
          fprintf(stderr, "%s: Invalid encoding: %s\n", g_get_prgname (), encoding);
          exit(1);
        }
    }

  text = read_file(IN, cvh);

  if (encoding != NULL && cvh != NULL)
    g_iconv_close(cvh);

  paragraphs = split_text_into_paragraphs(cr,
                                          pango_context,
                                          &page_layout,
                                          page_layout.column_width, 
                                          text);
  pango_lines = split_paragraphs_into_lines(&page_layout, paragraphs);

  cairo_scale(cr, page_layout.scale_x, page_layout.scale_y);

  output_pages(surface, cr, pango_lines, &page_layout, do_draw_header, pango_context);

  cairo_destroy (cr);
  cairo_surface_finish (surface);
  cairo_surface_destroy(surface);
  g_option_context_free(ctxt);

  return 0;
}


/* Read an entire file into a string
 */
static char *
read_file (FILE   *file,
           GIConv  handle)
{
  GString *inbuf;
  char *text;
  char buffer[BUFSIZE];

  inbuf = g_string_new (NULL);
  while (1)
    {
      char *ib, *ob, obuffer[BUFSIZE * 6], *bp = fgets (buffer, BUFSIZE-1, file);
      gsize iblen, oblen;

      if (ferror (file))
        {
          fprintf(stderr, "%s: Error reading file.\n", g_get_prgname ());
          g_string_free (inbuf, TRUE);
	  exit(1);
        }
      else if (bp == NULL)
        break;

      if (handle != NULL)
        {
          ib = bp;
          iblen = strlen (ib);
          ob = bp = obuffer;
          oblen = BUFSIZE * 6 - 1;
          if (g_iconv (handle, &ib, &iblen, &ob, &oblen) == (gsize)-1)
            {
              fprintf (stderr, "%s: Error while converting strings.\n", g_get_prgname ());
	      exit(1);
            }
          obuffer[BUFSIZE * 6 - 1 - oblen] = 0;
        }
      g_string_append (inbuf, bp);
    }

  fclose (file);

  /* Add a trailing new line if it is missing */
  if (inbuf->str[inbuf->len-1] != '\n')
    g_string_append(inbuf, "\n");

  text = inbuf->str;
  g_string_free (inbuf, FALSE);

  return text;
}


/* Take a UTF8 string and break it into paragraphs on \n characters
 */
static GList *
split_text_into_paragraphs (cairo_t *cr,
                            PangoContext *pango_context,
                            page_layout_t *page_layout,
                            int paint_width,  /* In pixels */
                            const char *text)
{
  const char *p = text;
  char *next;
  gunichar wc;
  GList *result = NULL;
  const char *last_para = text;

  /* If we are using markup we treat the entire text as a single paragraph.
   * I tested it and found that this is much slower than the split and
   * assign method used below. Otherwise we might as well use this single
   * chunk method always.
   */
  if (page_layout->do_use_markup)
    {
      Paragraph *para = g_new (Paragraph, 1);
      para->text = text;
      para->length = strlen(text);
      para->layout = pango_cairo_create_layout(cr); // pango_layout_new (pango_context);
      //          pango_layout_set_font_description (para->layout, font_description);
      pango_layout_set_markup (para->layout, para->text, para->length);
      pango_layout_set_justify (para->layout, page_layout->do_justify);
      pango_layout_set_alignment (para->layout,
                                  page_layout->pango_dir == PANGO_DIRECTION_LTR
                                      ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);
      if (page_layout->do_wordwrap) {
        pango_layout_set_wrap (para->layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width (para->layout, paint_width * PANGO_SCALE);
      } else {
        pango_layout_set_width (para->layout, -1);
      }
      para->height = 0;
      
      result = g_list_prepend (result, para);
    }
  else
    {

      while (p != NULL && *p)
        {
          wc = g_utf8_get_char (p);
          next = g_utf8_next_char (p);
          if (wc == (gunichar)-1)
            {
              fprintf (stderr, "%s: Invalid character in input\n", g_get_prgname ());
#ifdef HAVE_CUPS
	      if (page_layout->cups_mode)
		{
		  /* try to continue parsing text */
		  p = next;
		  continue;
		}
#endif
              wc = 0;
            }
          if (!*p || !wc || wc == '\n' || wc == '\f')
            {
              Paragraph *para = g_new (Paragraph, 1);
              para->text = last_para;
              para->length = p - last_para;
              para->layout = pango_layout_new (pango_context);
	      if (page_layout->cpi > 0.0L && page_layout->do_wordwrap)
		{
		  /* figuring out the correct width from the pango_font_metrics_get_approximate_width()
		   * is really hard and pango_layout_set_wrap() doesn't work properly then.
		   * Those are not reliable to render the characters exactly according to the given CPI.
		   * So re-calculate the widdth to wrap up to be comfortable with CPI.
		   */
		  wchar_t *wtext = NULL, *wnewtext = NULL;
		  gchar *newtext = NULL;
		  gsize len, col, i, wwidth = 0;
		  PangoRectangle ink_rect, logical_rect;

		  wtext = (wchar_t *)g_utf8_to_ucs4 (para->text, para->length, NULL, NULL, NULL);
		  if (wtext == NULL)
		    {
		      fprintf (stderr, "%s: Unable to convert UTF-8 to UCS-4.\n", g_get_prgname ());
		    fail:
		      g_free (wtext);
		      g_free (wnewtext);
		      g_free (newtext);
#ifdef HAVE_CUPS
		      if (page_layout->cups_mode)
			{
			  /* try to continue parsing text */
			  p = next;
			  continue;
			}
#endif
		      exit (1);
		    }
		  len = g_utf8_strlen (para->text, para->length);
		  /* the amount of characters that can be put on the line against CPI */
		  col = page_layout->column_width / 72.0 * page_layout->cpi;
		  if (len > col)
		    {
		      /* need to wrap them up */
		      wnewtext = g_new (wchar_t, wcslen (wtext) + 1);
		      if (wnewtext == NULL)
			{
			  fprintf (stderr, "%s: Unable to allocate the memory.\n", g_get_prgname ());
			  goto fail;
			}
		      for (i = 0; i < len; i++)
			{
			  gssize w = wcwidth (wtext[i]);

			  if (w >= 0)
			    wwidth += w;
			  if (wwidth > col)
			    break;
			  wnewtext[i] = wtext[i];
			}
		      wnewtext[i] = 0L;

		      newtext = g_ucs4_to_utf8 ((const gunichar *)wnewtext, i, NULL, NULL, NULL);
		      if (newtext == NULL)
			{
			  fprintf (stderr, "%s: Unable to convert UCS-4 to UTF-8.\n", g_get_prgname ());
			  goto fail;
			}
		      pango_layout_set_text (para->layout, newtext, -1);
		      pango_layout_get_extents (para->layout, &ink_rect, &logical_rect);
		      paint_width = logical_rect.width / PANGO_SCALE;
		      g_free (wnewtext);
		      g_free (newtext);

		      para->length = i;
		      next = g_utf8_offset_to_pointer (para->text, para->length);
		      wc = g_utf8_get_char (g_utf8_prev_char (next));
		    }
		  else
		    {
		      pango_layout_set_text (para->layout, para->text, para->length);
		    }
		  g_free (wtext);

		  pango_layout_set_width (para->layout, -1);
		}
	      else
		{
		  pango_layout_set_text (para->layout, para->text, para->length);
		  if (page_layout->do_wordwrap)
		    {
		      pango_layout_set_wrap (para->layout, PANGO_WRAP_WORD_CHAR);
		      pango_layout_set_width (para->layout, paint_width * PANGO_SCALE);
		    }
		  else
		    {
		      pango_layout_set_width (para->layout, -1);
		    }
		}
		  
              pango_layout_set_justify (para->layout, page_layout->do_justify);
              pango_layout_set_alignment (para->layout,
                                          page_layout->pango_dir == PANGO_DIRECTION_LTR
                                          ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT);

              para->height = 0;

              last_para = next;
            
              if (wc == '\f')
                para->formfeed = 1;
              else
                para->formfeed = 0;

              result = g_list_prepend (result, para);
            }
          if (!wc) /* incomplete character at end */
            break;
          p = next;
        }
    }

  return g_list_reverse (result);
}



/* Split a list of paragraphs into a list of lines.
 */
GList *
split_paragraphs_into_lines(page_layout_t *page_layout,
			    GList         *paragraphs)
{
  GList *line_list = NULL;
  int max_height = 0;
  /* Read the file */

  /* Now split all the pagraphs into lines */
  GList *par_list;

  par_list = paragraphs;
  while(par_list)
    {
      int para_num_lines, i;
      LineLink *line_link;
      Paragraph *para = par_list->data;

      para_num_lines = pango_layout_get_line_count(para->layout);

      for (i=0; i<para_num_lines; i++)
        {
          PangoRectangle logical_rect, ink_rect;
          
          line_link = g_new(LineLink, 1);
          line_link->formfeed = 0;
          line_link->pango_line = pango_layout_get_line(para->layout, i);
          pango_layout_line_get_extents(line_link->pango_line,
                                        &ink_rect, &logical_rect);
          line_link->logical_rect = logical_rect;
          if (para->formfeed && i == (para_num_lines - 1))
              line_link->formfeed = 1;
          line_link->ink_rect = ink_rect;
          line_list = g_list_prepend(line_list, line_link);
	  if (logical_rect.height > max_height)
	      max_height = logical_rect.height;
        }

      par_list = par_list->next;
    }
  
  if (page_layout->do_stretch_chars && page_layout->lpi > 0.0L)
      page_layout->scale_y = 1.0 / page_layout->lpi * 72.0 * PANGO_SCALE / max_height;

  return g_list_reverse(line_list);
  
}

int
output_pages(cairo_surface_t *surface,
             cairo_t       *cr,
             GList         *pango_lines,
             page_layout_t *page_layout,
             gboolean       need_header,
             PangoContext  *pango_context)
{
  int column_idx = 0;
  int column_y_pos = 0;
  int page_idx = 1;
  int pango_column_height = page_layout->column_height * PANGO_SCALE;
  int height = 0;
  LineLink *prev_line_link = NULL;

  start_page(surface, cr, page_layout, page_idx);

  if (need_header)
    draw_page_header_line_to_page(cr, FALSE, page_layout, pango_context, page_idx);

  while(pango_lines)
    {
      LineLink *line_link = pango_lines->data;
      PangoLayoutLine *line = line_link->pango_line;
      
      /* Check if we need to move to next column */
      if ((column_y_pos + line_link->logical_rect.height
           >= pango_column_height) ||
          (prev_line_link && prev_line_link->formfeed))
        {
          column_idx++;
          column_y_pos = 0;
          if (column_idx == page_layout->num_columns)
            {
              column_idx = 0;
              eject_page(cr);
              page_idx++;
              start_page(surface, cr, page_layout, page_idx);

              if (need_header)
                draw_page_header_line_to_page(cr, FALSE, page_layout, pango_context, page_idx);
            }
          else
            {
              eject_column(cr,
                           page_layout,
                           column_idx
                           );
            }
        }
      if (page_layout->lpi > 0.0L)
	height = (int)(1.0 / page_layout->lpi * 72.0 * PANGO_SCALE);
      else
	height = line_link->logical_rect.height;
      draw_line_to_page(cr,
                        column_idx,
                        column_y_pos+height,
                        page_layout,
                        line);
      column_y_pos += height;
      pango_lines = pango_lines->next;
      prev_line_link = line_link;
    }
  eject_page(cr);
  return page_idx;
}

void eject_column(cairo_t *cr,
                  page_layout_t *page_layout,
                  int column_idx)
{
  double x_pos, y_top, y_bot, total_gutter;

#if 0
  fprintf(stderr, "do_separation_line column_idx = %d %d\n", page_layout->do_separation_line, column_idx);
#endif
  if (!page_layout->do_separation_line)
    return;

  if (page_layout->pango_dir == PANGO_DIRECTION_RTL)
    column_idx = (page_layout->num_columns - column_idx);

  if (column_idx == 1)
    total_gutter = 1.0 * page_layout->gutter_width /2;
  else
    total_gutter = (column_idx - 0.5) * page_layout->gutter_width;
      
  x_pos = page_layout->left_margin
        + page_layout->column_width * column_idx
      + total_gutter;

  y_top = page_layout->top_margin + page_layout->header_height + page_layout->header_sep / 2;
  y_bot = page_layout->page_height - page_layout->bottom_margin - page_layout->footer_height;

  cairo_move_to(cr,x_pos, y_top);
  cairo_line_to(cr,x_pos, y_bot);
  cairo_set_line_width(cr, 0.1);
  cairo_stroke(cr);
}

void eject_page(cairo_t *cr)
{
  cairo_show_page(cr);
}

void start_page(cairo_surface_t *surface,
                cairo_t *cr,
                page_layout_t *page_layout,
                int page_idx)
{
  cairo_identity_matrix(cr);

  if (output_format == FORMAT_POSTSCRIPT)
    cairo_ps_surface_dsc_begin_page_setup (surface);

  if (page_layout->do_landscape)
    {
      if (output_format == FORMAT_POSTSCRIPT)
        {
          cairo_ps_surface_dsc_comment (surface, "%%PageOrientation: Landscape");
          cairo_translate(cr,page_layout->page_height, 0);
          cairo_rotate(cr, M_PI/2);
        }
    }
  else
    {
      if (output_format == FORMAT_POSTSCRIPT)
        cairo_ps_surface_dsc_comment (surface, "%%PageOrientation: Portrait");
    }
}

void
draw_line_to_page(cairo_t *cr,
                  int column_idx,
                  int column_pos,
                  page_layout_t *page_layout,
                  PangoLayoutLine *line)
{
  /* Assume square aspect ratio for now */
  double y_pos = page_layout->top_margin
               + page_layout->header_sep
               + column_pos / PANGO_SCALE;
  double x_pos = page_layout->left_margin
               + column_idx * (page_layout->column_width
                               + page_layout->gutter_width);
  PangoRectangle ink_rect, logical_rect;

  /* Do RTL column layout for RTL direction */
  if (page_layout->pango_dir == PANGO_DIRECTION_RTL)
    {
      x_pos = page_layout->left_margin
        + (page_layout->num_columns-1-column_idx)
        * (page_layout->column_width + page_layout->gutter_width);
    }
  
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);

  if (page_layout->pango_dir == PANGO_DIRECTION_RTL) {
      x_pos += page_layout->column_width  - logical_rect.width / PANGO_SCALE;
  }

  cairo_move_to(cr, x_pos, y_pos);
  pango_cairo_show_layout_line(cr, line);
}

int
draw_page_header_line_to_page(cairo_t         *cr,
                              gboolean         is_footer,
                              page_layout_t   *page_layout,
                              PangoContext    *ctx,
                              int              page)
{
  PangoLayout *layout = pango_layout_new(ctx);
  PangoLayoutLine *line;
  PangoRectangle ink_rect, logical_rect;
  /* Assume square aspect ratio for now */
  double x_pos, y_pos;
  gchar *header, date[256];
  time_t t;
  struct tm tm;
  int height;
  gdouble line_pos;

  /* Reset gravity?? */

  t = time(NULL);
  tm = *localtime(&t);
  strftime(date, 255, "%c", &tm);
  header = g_strdup_printf("<span font_desc=\"%s\">%s</span>\n"
                           "<span font_desc=\"%s\">%s</span>\n"
                           "<span font_desc=\"%s\">Page %d</span>",
                           page_layout->header_font_desc,
                           date,
                           page_layout->header_font_desc,
                           page_layout->filename,
                           page_layout->header_font_desc,
                           page);
  pango_layout_set_markup(layout, header, -1);
  g_free(header);

  /* output a left edge of header/footer */
  line = pango_layout_get_line(layout, 0);
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);
  x_pos = page_layout->left_margin;
  height = logical_rect.height / PANGO_SCALE /3.0;

  /* The header is placed right after the margin */
  if (is_footer)
    {
      y_pos = page_layout->page_height - page_layout->bottom_margin;
      page_layout->footer_height = height;
    }
  else
    {
      y_pos = page_layout->top_margin + height;
      page_layout->header_height = height;
    }

  cairo_move_to(cr, x_pos,y_pos);
  pango_cairo_show_layout_line(cr,line);

  /* output a center of header/footer */
  line = pango_layout_get_line(layout, 1);
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);
  x_pos = (page_layout->page_width - (logical_rect.width / PANGO_SCALE )) / 2;
  cairo_move_to(cr, x_pos,y_pos);
  pango_cairo_show_layout_line(cr,line);

  /* output a right edge of header/footer */
  line = pango_layout_get_line(layout, 2);
  pango_layout_line_get_extents(line,
                                &ink_rect,
                                &logical_rect);
  x_pos = page_layout->page_width - page_layout->right_margin - (logical_rect.width / PANGO_SCALE );
  cairo_move_to(cr, x_pos,y_pos);
  pango_cairo_show_layout_line(cr,line);

  g_object_unref(layout);

  /* header separator */
  line_pos = page_layout->top_margin + page_layout->header_height + page_layout->header_sep / 2;
  cairo_move_to(cr, page_layout->left_margin, line_pos);
  cairo_line_to(cr,page_layout->page_width - page_layout->right_margin, line_pos);
  cairo_set_line_width(cr,0.1); // TBD
  cairo_stroke(cr);

  return logical_rect.height;
}
