#pragma once

#include "pictures.h"

/* Pictures shared between displays through the update site (see share.c). */

/* The /bilder page's sharing requests, and the downloads by themselves. */
void share_start(void);

/* Who shared the downloaded pictures: "<id>\t<nickname>" lines
 * (share_nick_line), only those with a nickname. */
#define SHARE_WHO_FILE PICTURES_DIR "/DELT.TXT"

/* Shared picture `id` deleted from this display's card on the /bilder
 * page: not downloaded again (SD lock held). */
void share_hide(const char *id);
