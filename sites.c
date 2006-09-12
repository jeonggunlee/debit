/*
 * (C) Copyright 2006 Jean-Baptiste Note <jean-baptiste.note@m4x.org>
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "keyfile.h"

#include "sites.h"

/*
 * File site control description is of form
 * [XC2V2000]
 * X_WIDTH=;
 * Y_WIDTH=;
 * File site description data is of this form
 * [SITENAME]
 * ID=NUM;
 * XINTERVALS=1,2,3,4
 * YINTERVALS=2,3,7,7
 * This means: sites in ([1,2] u [3,4]) x ([2,3] u [7,7]) are of type ID
 * Xilinx is cool: the names are okay so that this description works and
 * we don't have to repeat sitename several times
 */

/* iterate over intervals */
typedef void (* interval_iterator_t)(unsigned i, void *dat);

static void
iterate_over_intervals(interval_iterator_t iter, void *dat,
		       const gint *intervals, const gsize len) {
  unsigned i;
  for (i = 0; i < len-1; i+=2) {
    unsigned j,
      min = intervals[i],
      max = intervals[i+1];
    /* iterate over the interval */
    for (j = min; j < max; j++)
      iter(j, dat);
  }
}

typedef struct _line_iterator {
  chip_descr_t *chip;
  site_type_t type;
  const gint *intervals_x;
  unsigned xlen;
  /* current line we're operating at */
  unsigned y;
} line_iterator_t;

static void
pos_setting(unsigned x, void *dat) {
  line_iterator_t *data = dat;
  unsigned y = data->y;
  chip_descr_t *chip = data->chip;
  site_type_t type = data->type;
  csite_descr_t *descr = get_global_site(chip,x,y);

  g_assert(descr->type == SITE_TYPE_NEUTRAL);
  descr->type = type;
}

static void
iterate_over_lines(unsigned y, void *dat) {
  /* dat contains the y-coordinate */
  line_iterator_t *data = dat;
  data->y = y;
  iterate_over_intervals(pos_setting, dat,
			 data->intervals_x, data->xlen);
}

static void
init_chip_type(chip_descr_t *chip, site_type_t type,
	       const gint *intervals_x, const gsize xlen,
	       const gint *intervals_y, const gsize ylen) {
  line_iterator_t x_arg = {
    .type = type, .chip = chip,
    .intervals_x = intervals_x, .xlen = xlen,
  };
  iterate_over_intervals(iterate_over_lines, &x_arg,
			 intervals_y, ylen);
}

static inline void
alloc_chip(chip_descr_t *descr) {
  unsigned nelems = descr->width * descr->height;
  descr->data = g_new0(csite_descr_t, nelems);
}

static void
init_group_chip_type(GKeyFile *file, const gchar *group,
		     gpointer data) {
  chip_descr_t *chip = data;
  GError *error = NULL;
  gint *intervals[2];
  gsize sizes[2];
  unsigned type;

  /* get group name & al from the group, fill in with this */
  intervals[0] = g_key_file_get_integer_list(file, group, "x", &sizes[0], &error);
  if (error)
    goto out_err;
  intervals[1] = g_key_file_get_integer_list(file, group, "y", &sizes[1], &error);
  if (error)
    goto out_err;
  type = g_key_file_get_integer(file, group, "type", &error);
  if (error)
    goto out_err;
  g_assert(type < NR_SITE_TYPE);
  init_chip_type(chip, type, intervals[0], sizes[0], intervals[1], sizes[1]);
  return;

 out_err:
  g_warning("Error treating group %s: %s",
	    group, error->message);
  g_error_free(error);
  return;
}

void
iterate_over_sites(const chip_descr_t *chip,
		   site_iterator_t fun, gpointer data) {
  unsigned x, y;
  unsigned xmax = chip->width, ymax = chip->height;
  csite_descr_t *site = chip->data;

  for (y = 0; y < ymax; y++)
    for (x = 0; x < xmax; x++)
      fun(x,y,site++,data);
}

static void
init_site_default(unsigned site_x, unsigned site_y,
		  csite_descr_t *site, gpointer dat) {
  (void) dat;
  (void) site_x;
  (void) site_y;
  site->type = SITE_TYPE_NEUTRAL;
}

static inline void
init_default_values(chip_descr_t *chip) {
  iterate_over_sites(chip, init_site_default, NULL);
}

typedef struct _local_counter {
  gint x;
  gint y;
  gint y_global;
} local_counter_t;

static void
init_site_coord(unsigned site_x, unsigned site_y,
		csite_descr_t *site, gpointer dat) {
  local_counter_t *coords = dat;
  site_type_t type = site->type;
  local_counter_t *count = coords + type;
  gint x = count->x, y = count->y, y_global = count->y_global;
  gboolean newline = y_global < (int)site_y ? TRUE : FALSE;

  if (newline) {
    y++;
    x = 0;
  } else {
    x++;
  }

  site->type_coord.x = x;
  site->type_coord.y = y;

  if (newline)
    count->y_global = site_y;
  count->x = x;
  count->y = y;
}

static void
init_local_coordinates(chip_descr_t *chip) {
  /* local counters */
  local_counter_t coords[NR_SITE_TYPE];
  unsigned i;
  for (i = 0; i < NR_SITE_TYPE; i++) {
    coords[i].y = -1;
    coords[i].y_global = -1;
  }
  iterate_over_sites(chip, init_site_coord, coords);
}

static void
init_chip(chip_descr_t *chip, GKeyFile *file) {
  /* for each of the types, call the init functions */
  init_default_values(chip);
  iterate_over_groups(file, init_group_chip_type, chip);
  init_local_coordinates(chip);
}

/* exported alloc and destroy functions */
chip_descr_t *
get_chip(const gchar *dirname, const gchar *chipname) {
  chip_descr_t *chip = g_new0(chip_descr_t, 1);
  GKeyFile *keyfile;
  GError *error = NULL;
  gchar *filename;
  int err;

  filename = g_build_filename(dirname, chipname, "chip_control", NULL);
  err = read_keyfile(&keyfile,filename);
  g_free(filename);
  if (err)
    goto out_err;

#define DIM "DIMENTIONS"
  /* Get width and height */
  chip->width  = g_key_file_get_integer(keyfile, DIM, "WIDTH", &error);
  if (error)
    goto out_err_free_err_keyfile;
  chip->height = g_key_file_get_integer(keyfile, DIM, "HEIGHT", &error);
  if (error)
    goto out_err_free_err_keyfile;

  alloc_chip(chip);
  g_key_file_free(keyfile);

  filename = g_build_filename(dirname, chipname, "chip_data", NULL);
  err = read_keyfile(&keyfile,filename);
  g_free(filename);
  if (err)
    goto out_err;

  init_chip(chip, keyfile);
  g_key_file_free(keyfile);

  return chip;

 out_err_free_err_keyfile:
  g_warning("error reading chip description: %s", error->message);
  g_error_free(error);
  g_key_file_free(keyfile);
 out_err:
  g_free(chip);
  return NULL;
}

void
release_chip(chip_descr_t *chip) {
  g_free(chip->data);
  chip->data = NULL;
  g_free(chip);
}

/*
 * Utility function
 */

void
sprint_csite(gchar *data, const csite_descr_t *site) {
  /* Use a string chunk ? */
  const guint x = site->type_coord.x + 1;
  const guint y = site->type_coord.y + 1;

  switch (site->type) {
  case CLB:
    sprintf(data, "R%iC%i", y, x);
    break;
  case RTERM:
    sprintf(data, "RTERMR%i", y);
    break;
  case LTERM:
    sprintf(data, "LTERMR%i", y);
    break;
  case TTERM:
    sprintf(data, "TTERMC%i", x);
    break;
  case BTERM:
    sprintf(data, "BTERMC%i", x);
    break;
  case TIOI:
    sprintf(data, "TIOIC%i", x);
    break;
  case BIOI:
    sprintf(data, "BIOIC%i", x);
    break;
  case LIOI:
    sprintf(data, "LIOIR%i", y);
    break;
  case RIOI:
    sprintf(data, "RIOIR%i", y);
    break;
  case TTERMBRAM:
    sprintf(data, "TTERMBRAMC%i", x);
    break;
  case BTERMBRAM:
    sprintf(data, "BTERMBRAMC%i", x);
    break;
  case TIOIBRAM:
    sprintf(data, "TIOIBRAMC%i", x);
    break;
  case BIOIBRAM:
    sprintf(data, "BIOIBRAMC%i", x);
    break;
  case BRAM:
    sprintf(data, "BRAMR%iC%i", y, x);
    break;
  default:
    sprintf(data, "GLOBALR%iC%i", y, x);
    break;
  }
}

static void
print_iterator(unsigned x, unsigned y,
	       csite_descr_t *site, gpointer dat) {
  gchar name[32];
  sprint_csite(name, site);
  (void) dat;
  g_print("Global site (%i,%i) is %s\n", x, y, name);
}

void
print_chip(chip_descr_t *chip) {
  iterate_over_sites(chip, print_iterator, NULL);
}
