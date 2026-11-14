/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QtGlobal>

/*
 * Single DES (FIPS 46-3), encryption of one block. Only for VNC
 * authentication, which is built on it; DES has long been unfit to protect
 * anything else.
 */
void desEncryptBlock(const quint8 key[8], quint8 block[8]);
