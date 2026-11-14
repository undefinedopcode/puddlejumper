/*
 * superterm — Qt Widgets port.
 *
 * Distributed under the terms of the GNU General Public License v2 or later.
 */

#pragma once

#include <QByteArray>
#include <QtGlobal>

/*
 * The cryptography VNC authentication is built on, which Qt doesn't have:
 * enough for VncClient, and not meant for anything else (DES has long been
 * unfit to protect anything; none of this is constant-time).
 */

/* Single DES (FIPS 46-3): encrypt one block. For VNC password authentication. */
void desEncryptBlock(const quint8 key[8], quint8 block[8]);

/* AES-128 (FIPS 197): encrypt one block. For Apple's (ARD) authentication. */
void aes128EncryptBlock(const quint8 key[16], quint8 block[16]);

/*
 * base^exponent mod modulus, all as big-endian unsigned numbers; the result
 * is as many bytes as the modulus. For the Diffie-Hellman exchange of
 * Apple's authentication. The modulus must be odd (a prime is) and the base
 * less than it; else the result is empty.
 */
QByteArray modPow(const QByteArray &base, const QByteArray &exponent, const QByteArray &modulus);
