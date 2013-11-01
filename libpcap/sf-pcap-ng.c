/*
 * Copyright (c) 2012-2013 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1993, 1994, 1995, 1996, 1997
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution, and (3) all advertising materials mentioning
 * features or use of this software display the following acknowledgement:
 * ``This product includes software developed by the University of California,
 * Lawrence Berkeley Laboratory and its contributors.'' Neither the name of
 * the University nor the names of its contributors may be used to endorse
 * or promote products derived from this software without specific prior
 * written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * sf-pcap-ng.c - pcap-ng-file-format-specific code from savefile.c
 */

#ifndef lint
static const char rcsid[] _U_ =
"@(#) $Header$ (LBL)";
#endif


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#include <pcap-stdinc.h>
#else /* WIN32 */
#if HAVE_INTTYPES_H
#include <inttypes.h>
#elif HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_SYS_BITYPES_H
#include <sys/bitypes.h>
#endif
#include <sys/types.h>
#endif /* WIN32 */

#include <errno.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/utsname.h>

#define PCAP_DONT_INCLUDE_PCAP_BPF_H
#include "bpf.h"

#include "pcap-int.h"

#include "pcap-common.h"

#include "pcap-util.h"

#ifdef HAVE_OS_PROTO_H
#include "os-proto.h"
#endif

#include "sf-pcap-ng.h"

/*
 * Block types.
 */
#include "pcap-ng.h"

/*
 * Section Header Block.
 */
#define BT_SHB			0x0A0D0D0A


/*
 * Byte-order magic value.
 */
#define BYTE_ORDER_MAGIC	0x1A2B3C4D

/*
 * Current version number.  If major_version isn't PCAPNG_VERSION_MAJOR,
 * that means that this code can't read the file.
 */
#define PCAPNG_VERSION_MAJOR	1

/*
 * Interface Description Block.
 */
#define BT_IDB			0x00000001

/*
 * Options in the IDB.
 */
#define IF_NAME		2	/* interface name string */
#define IF_DESCRIPTION	3	/* interface description string */
#define IF_IPV4ADDR	4	/* interface's IPv4 address and netmask */
#define IF_IPV6ADDR	5	/* interface's IPv6 address and prefix length */
#define IF_MACADDR	6	/* interface's MAC address */
#define IF_EUIADDR	7	/* interface's EUI address */
#define IF_SPEED	8	/* interface's speed, in bits/s */
#define IF_TSRESOL	9	/* interface's time stamp resolution */
#define IF_TZONE	10	/* interface's time zone */
#define IF_FILTER	11	/* filter used when capturing on interface */
#define IF_OS		12	/* string OS on which capture on this interface was done */
#define IF_FCSLEN	13	/* FCS length for this interface */
#define IF_TSOFFSET	14	/* time stamp offset for this interface */

/*
 * Enhanced Packet Block.
 */
#define BT_EPB			0x00000006

/*
 * Simple Packet Block.
 */
#define BT_SPB			0x00000003


/*
 * Packet Block.
 */
#define BT_PB			0x00000002


static int pcap_ng_next_packet(pcap_t *p, struct pcap_pkthdr *hdr,
							   u_char **data);

static int pcap_ng_next_block(pcap_t *p, struct pcap_pkthdr *hdr,
							  u_char **data);

static int
read_bytes(FILE *fp, void *buf, size_t bytes_to_read, int fail_on_eof,
		   char *errbuf)
{
	size_t amt_read;
	
	amt_read = fread(buf, 1, bytes_to_read, fp);
	if (amt_read != bytes_to_read) {
		if (ferror(fp)) {
			snprintf(errbuf, PCAP_ERRBUF_SIZE,
					 "error reading dump file: %s",
					 pcap_strerror(errno));
		} else {
			if (amt_read == 0 && !fail_on_eof)
				return (0);	/* EOF */
			snprintf(errbuf, PCAP_ERRBUF_SIZE,
					 "truncated dump file; tried to read %lu bytes, only got %lu",
					 (unsigned long)bytes_to_read,
					 (unsigned long)amt_read);
		}
		return (-1);
	}
	return (1);
}

static int
read_block(FILE *fp, pcap_t *p, struct block_cursor *cursor, char *errbuf)
{
	int status;
	struct pcapng_block_header bhdr;
	
	status = read_bytes(fp, &bhdr, sizeof(bhdr), 0, errbuf);
	if (status <= 0)
		return (status);	/* error or EOF */
	
	if (p->sf.swapped) {
		bhdr.block_type = SWAPLONG(bhdr.block_type);
		bhdr.total_length = SWAPLONG(bhdr.total_length);
	}
	
	/*
	 * Is this block "too big"?
	 *
	 * We choose 16MB as "too big", for now, so that we handle
	 * "reasonably" large buffers but don't chew up all the
	 * memory if we read a malformed file.
	 */
	if (bhdr.total_length > 16*1024*1024) {
		snprintf(errbuf, PCAP_ERRBUF_SIZE,
				 "pcap-ng block size %u > maximum %u",
				 bhdr.total_length, 16*1024*1024);
		return (-1);
	}
	
	/*
	 * Is this block "too small" - i.e., is it shorter than a block
	 * header plus a block trailer?
	 */
	if (bhdr.total_length < sizeof(struct pcapng_block_header) +
		sizeof(struct pcapng_block_trailer)) {
		snprintf(errbuf, PCAP_ERRBUF_SIZE,
				 "block in pcap-ng dump file has a length of %u < %lu",
				 bhdr.total_length,
				 (unsigned long)(sizeof(struct pcapng_block_header) + sizeof(struct pcapng_block_trailer)));
		return (-1);
	}
	
	/*
	 * Some ntar files from wireshark.org do not round up the total block length to
	 * a multiple of 4 bytes -- they must ignore the 32 bit alignment of the block body!
	 */
	if (bhdr.total_length % 4 != 0)
		bhdr.total_length += 4 - (bhdr.total_length % 4);
	
	/*
	 * Is the buffer big enough?
	 */
	if (p->bufsize < bhdr.total_length) {
		/*
		 * No - make it big enough.
		 */
		p->buffer = realloc(p->buffer, bhdr.total_length);
		if (p->buffer == NULL) {
			snprintf(errbuf, PCAP_ERRBUF_SIZE, "out of memory");
			return (-1);
		}
	}
	
	/*
	 * Copy the stuff we've read to the buffer, and read the rest
	 * of the block.
	 */
	memcpy(p->buffer, &bhdr, sizeof(bhdr));
	if (read_bytes(fp, p->buffer + sizeof(bhdr),
				   bhdr.total_length - sizeof(bhdr), 1, errbuf) == -1)
		return (-1);
	
	/*
	 * Initialize the cursor.
	 */
	cursor->data = p->buffer + sizeof(bhdr);
	cursor->data_remaining = bhdr.total_length - sizeof(bhdr) -
	sizeof(struct pcapng_block_trailer);
	cursor->block_type = bhdr.block_type;
	return (1);
}

void *
get_from_block_data(struct block_cursor *cursor, size_t chunk_size,
					char *errbuf)
{
	void *data;
	
	/*
	 * Make sure we have the specified amount of data remaining in
	 * the block data.
	 */
	if (cursor->data_remaining < chunk_size) {
		if (errbuf)
			snprintf(errbuf, PCAP_ERRBUF_SIZE,
					 "block of type %u in pcap-ng dump file is too short",
					 cursor->block_type);
		return (NULL);
	}
	
	/*
	 * Return the current pointer, and skip past the chunk.
	 */
	data = cursor->data;
	cursor->data += chunk_size;
	cursor->data_remaining -= chunk_size;
	return (data);
}

struct pcapng_option_header *
get_opthdr_from_block_data(struct pcapng_option_header *opthdr, int swapped,
						   struct block_cursor *cursor, char *errbuf)
{
	struct pcapng_option_header *optp;
	
	optp = get_from_block_data(cursor, sizeof(*opthdr), errbuf);
	if (optp == NULL) {
		/*
		 * Option header is cut short.
		 */
		return (NULL);
	}
	*opthdr = *optp;
	/*
	 * Byte-swap it if necessary.
	 */
	if (swapped) {
		opthdr->option_code = SWAPSHORT(opthdr->option_code);
		opthdr->option_length = SWAPSHORT(opthdr->option_length);
	}
	
	return (opthdr);
}

void *
get_optvalue_from_block_data(struct block_cursor *cursor,
							 struct pcapng_option_header *opthdr, char *errbuf)
{
	size_t padded_option_len;
	void *optvalue;
	
	/* Pad option length to 4-byte boundary */
	padded_option_len = opthdr->option_length;
	padded_option_len = ((padded_option_len + 3)/4)*4;
	
	optvalue = get_from_block_data(cursor, padded_option_len, errbuf);
	if (optvalue == NULL) {
		/*
		 * Option value is cut short.
		 */
		return (NULL);
	}
	
	return (optvalue);
}

static int
process_idb_options(pcap_t *p, struct pcapng_interface_description_fields *idbp,
					struct block_cursor *cursor, u_int *tsresol,
					u_int64_t *tsoffset, char *errbuf)
{
	struct pcapng_option_header opthdr;
	void *optvalue;
	int saw_tsresol, saw_tsoffset, saw_ifname;
	u_char tsresol_opt;
	u_int i;
		
	saw_tsresol = 0;
	saw_tsoffset = 0;
	saw_ifname = 0;
	while (cursor->data_remaining != 0) {
		/*
		 * Get the option header.
		 */
		if (get_opthdr_from_block_data(&opthdr, p->sf.swapped, cursor, errbuf) == NULL) {
			/*
			 * Option header is cut short.
			 */
			goto fail;	/* error */
		}
		
		/*
		 * Get option value.
		 */
		optvalue = get_optvalue_from_block_data(cursor, &opthdr,
												errbuf);
		if (optvalue == NULL) {
			/*
			 * Option value is cut short.
			 */
			goto fail;	/* error */
		}
		
		switch (opthdr.option_code) {
				
			case PCAPNG_IF_NAME:
				if (saw_ifname) {
					snprintf(errbuf, PCAP_ERRBUF_SIZE,
							 "Interface Description Block has more than one if_name option");
					goto fail;	/* error */
				}
				saw_ifname = 1;
				break;
				
			case PCAPNG_OPT_ENDOFOPT:
				if (opthdr.option_length != 0) {
					snprintf(errbuf, PCAP_ERRBUF_SIZE,
							 "Interface Description Block has opt_endofopt option with length %u != 0",
							 opthdr.option_length);
					goto fail;	/* error */
				}
				goto done;
				
			case PCAPNG_IF_TSRESOL:
				if (opthdr.option_length != 1) {
					snprintf(errbuf, PCAP_ERRBUF_SIZE,
							 "Interface Description Block has if_tsresol option with length %u != 1",
							 opthdr.option_length);
					goto fail;	/* error */
				}
				if (saw_tsresol) {
					snprintf(errbuf, PCAP_ERRBUF_SIZE,
							 "Interface Description Block has more than one if_tsresol option");
					goto fail;	/* error */
				}
				saw_tsresol = 1;
				tsresol_opt = *(u_int *)optvalue;
				if (tsresol_opt & 0x80) {
					/*
					 * Resolution is negative power of 2.
					 */
					*tsresol = 1 << (tsresol_opt & 0x7F);
				} else {
					/*
					 * Resolution is negative power of 10.
					 */
					*tsresol = 1;
					for (i = 0; i < tsresol_opt; i++)
						*tsresol *= 10;
				}
				if (*tsresol == 0) {
					/*
					 * Resolution is too high.
					 */
					if (tsresol_opt & 0x80) {
						snprintf(errbuf, PCAP_ERRBUF_SIZE,
								 "Interface Description Block if_tsresol option resolution 2^-%u is too high",
								 tsresol_opt & 0x7F);
					} else {
						snprintf(errbuf, PCAP_ERRBUF_SIZE,
								 "Interface Description Block if_tsresol option resolution 10^-%u is too high",
								 tsresol_opt);
					}
					goto fail;	/* error */
				}
				break;
				
			case PCAPNG_IF_TSOFFSET:
				if (opthdr.option_length != 8) {
					snprintf(errbuf, PCAP_ERRBUF_SIZE,
							 "Interface Description Block has if_tsoffset option with length %u != 8",
							 opthdr.option_length);
					goto fail;	/* error */
				}
				if (saw_tsoffset) {
					snprintf(errbuf, PCAP_ERRBUF_SIZE,
							 "Interface Description Block has more than one if_tsoffset option");
					goto fail;	/* error */
				}
				saw_tsoffset = 1;
				memcpy(tsoffset, optvalue, sizeof(*tsoffset));
				if (p->sf.swapped)
					*tsoffset = SWAPLL(*tsoffset);
				break;
				
			default:
				break;
		}
	}
done:
	/*
	 * Count this interface.
	 */
	p->ifcount++;
	
	/*
	 * Compute the scaling factor to convert the
	 * sub-second part of the time stamp to
	 * microseconds.
	 */
	if (p->sf.tsresol > 1000000) {
		/*
		 * Higher than microsecond resolution;
		 * scale down to microseconds.
		 */
		p->sf.tsscale = (p->sf.tsresol / 1000000);
	} else {
		/*
		 * Lower than microsecond resolution;
		 * scale up to microseconds.
		 */
		p->sf.tsscale = (1000000 / p->sf.tsresol);
	}
	
	return (0);
fail:
	return (-1);
}


/*
 * Check whether this is a pcap-ng savefile and, if it is, extract the
 * relevant information from the header.
 */
int
pcap_ng_check_header(pcap_t *p, bpf_u_int32 magic, FILE *fp, char *errbuf, int isng)
{
	size_t amt_read;
	bpf_u_int32 total_length;
	bpf_u_int32 byte_order_magic;
	struct pcapng_block_header *bhdrp;
	struct pcapng_section_header_fields *shbp;
	int status;
	struct block_cursor cursor;
	struct pcapng_interface_description_fields *idbp;
	long file_offset = ftell(fp);
	
	/*
	 * Check whether the first 4 bytes of the file are the block
	 * type for a pcap-ng savefile.
	 */
	if (magic != PCAPNG_BT_SHB) {
		/*
		 * XXX - check whether this looks like what the block
		 * type would be after being munged by mapping between
		 * UN*X and DOS/Windows text file format and, if it
		 * does, look for the byte-order magic number in
		 * the appropriate place and, if we find it, report
		 * this as possibly being a pcap-ng file transferred
		 * between UN*X and Windows in text file format?
		 */
		return (0);	/* nope */
	}
	
	/*
	 * OK, they are.  However, that's just \n\r\r\n, so it could,
	 * conceivably, be an ordinary text file.
	 *
	 * It could not, however, conceivably be any other type of
	 * capture file, so we can read the rest of the putative
	 * Section Header Block; put the block type in the common
	 * header, read the rest of the common header and the
	 * fixed-length portion of the SHB, and look for the byte-order
	 * magic value.
	 */
	amt_read = fread(&total_length, 1, sizeof(total_length), fp);
	if (amt_read < sizeof(total_length)) {
		if (ferror(fp)) {
			snprintf(errbuf, PCAP_ERRBUF_SIZE,
					 "error reading dump file: %s",
					 pcap_strerror(errno));
			return (-1);	/* fail */
		}
		
		/*
		 * Possibly a weird short text file, so just say
		 * "not pcap-ng".
		 */
		return (0);
	}
	amt_read = fread(&byte_order_magic, 1, sizeof(byte_order_magic), fp);
	if (amt_read < sizeof(byte_order_magic)) {
		if (ferror(fp)) {
			snprintf(errbuf, PCAP_ERRBUF_SIZE,
					 "error reading dump file: %s",
					 pcap_strerror(errno));
			return (-1);	/* fail */
		}
		
		/*
		 * Possibly a weird short text file, so just say
		 * "not pcap-ng".
		 */
		return (0);
	}
	if (byte_order_magic != PCAPNG_BYTE_ORDER_MAGIC) {
		byte_order_magic = SWAPLONG(byte_order_magic);
		if (byte_order_magic != PCAPNG_BYTE_ORDER_MAGIC) {
			/*
			 * Not a pcap-ng file.
			 */
			return (0);
		}
		p->sf.swapped = 1;
		total_length = SWAPLONG(total_length);
	}
	
	/*
	 * Check the sanity of the total length.
	 */
	if (total_length < sizeof(*bhdrp) + sizeof(*shbp) + sizeof(struct pcapng_block_trailer)) {
		snprintf(errbuf, PCAP_ERRBUF_SIZE,
				 "Section Header Block in pcap-ng dump file has a length of %u < %lu",
				 total_length,
				 (unsigned long)(sizeof(*bhdrp) + sizeof(*shbp) + sizeof(struct pcapng_block_trailer)));
		return (-1);
	}
	
	/*
	 * Allocate a buffer into which to read blocks.  We default to
	 * the maximum of:
	 *
	 *	the total length of the SHB for which we read the header;
	 *
	 *	2K, which should be more than large enough for an Enhanced
	 *	Packet Block containing a full-size Ethernet frame, and
	 *	leaving room for some options.
	 *
	 * If we find a bigger block, we reallocate the buffer.
	 */
	p->bufsize = 2048;
	if (p->bufsize < total_length)
		p->bufsize = total_length;
	p->buffer = malloc(p->bufsize);
	if (p->buffer == NULL) {
		snprintf(errbuf, PCAP_ERRBUF_SIZE, "out of memory");
		return (-1);
	}
	
	/*
	 * Copy the stuff we've read to the buffer, and read the rest
	 * of the SHB.
	 */
	bhdrp = (struct pcapng_block_header *)p->buffer;
	shbp = (struct pcapng_section_header_fields *)(p->buffer + sizeof(struct pcapng_block_header));
	bhdrp->block_type = magic;
	bhdrp->total_length = total_length;
	shbp->byte_order_magic = byte_order_magic;
	if (read_bytes(fp,
				   p->buffer + (sizeof(magic) + sizeof(total_length) + sizeof(byte_order_magic)),
				   total_length - (sizeof(magic) + sizeof(total_length) + sizeof(byte_order_magic)),
				   1, errbuf) == -1)
		goto fail;
	
	if (p->sf.swapped) {
		/*
		 * Byte-swap the fields we've read.
		 */
		shbp->major_version = SWAPSHORT(shbp->major_version);
		shbp->minor_version = SWAPSHORT(shbp->minor_version);
		
		/*
		 * XXX - we don't care about the section length.
		 */
	}
	if (shbp->major_version != PCAPNG_VERSION_MAJOR) {
		snprintf(errbuf, PCAP_ERRBUF_SIZE,
				 "unknown pcap-ng savefile major version number %u",
				 shbp->major_version);
		goto fail;
	}
	p->sf.version_major = shbp->major_version;
	p->sf.version_minor = shbp->minor_version;
	
	/*
	 * Set the default time stamp resolution and offset.
	 */
	p->sf.tsresol = 1000000;	/* microsecond resolution */
	p->sf.tsscale = 1;		/* multiply by 1 to scale to microseconds */
	p->sf.tsoffset = 0;		/* absolute timestamps */
	
	/*
	 * Now start looking for an Interface Description Block.
	 */
	for (;;) {
		/*
		 * Read the next block.
		 */
		status = read_block(fp, p, &cursor, errbuf);
		if (status == 0) {
			/* EOF - no IDB in this file */
			snprintf(errbuf, PCAP_ERRBUF_SIZE,
					 "the capture file has no Interface Description Blocks");
			goto fail;
		}
		if (status == -1)
			goto fail;	/* error */
		switch (cursor.block_type) {
				
			case PCAPNG_BT_IDB:
				/*
				 * Get a pointer to the fixed-length portion of the
				 * IDB.
				 */
				idbp = get_from_block_data(&cursor, sizeof(*idbp),
										   errbuf);
				if (idbp == NULL)
					goto fail;	/* error */
				
				/*
				 * Byte-swap it if necessary.
				 */
				if (p->sf.swapped) {
					idbp->linktype = SWAPSHORT(idbp->linktype);
					idbp->snaplen = SWAPLONG(idbp->snaplen);
				}
				
				/*
				 * Now look for various time stamp options, so
				 * we know how to interpret the time stamps.
				 */
				if (process_idb_options(p, idbp, &cursor, &p->sf.tsresol,
										&p->sf.tsoffset, errbuf) == -1)
					goto fail;
				
				/*
				 * Compute the scaling factor to convert the
				 * sub-second part of the time stamp to
				 * microseconds.
				 */
				if (p->sf.tsresol > 1000000) {
					/*
					 * Higher than microsecond resolution;
					 * scale down to microseconds.
					 */
					p->sf.tsscale = (p->sf.tsresol / 1000000);
				} else {
					/*
					 * Lower than microsecond resolution;
					 * scale up to microseconds.
					 */
					p->sf.tsscale = (1000000 / p->sf.tsresol);
				}
				p->tzoff = 0;	/* XXX - not used in pcap */
				p->snapshot = idbp->snaplen;
				p->linktype = linktype_to_dlt(idbp->linktype);
				p->linktype_ext = 0;
				goto done;
				
			case PCAPNG_BT_EPB:
			case PCAPNG_BT_SPB:
			case PCAPNG_BT_PB:
				/*
				 * Saw a packet before we saw any IDBs.  That's
				 * not valid, as we don't know what link-layer
				 * encapsulation the packet has.
				 */
				snprintf(errbuf, PCAP_ERRBUF_SIZE,
						 "the capture file has a packet block before any Interface Description Blocks");
				goto fail;
				
			default:
				/*
				 * Just ignore it.
				 */
				break;
		}
	}
	
done:
	p->sf.next_packet_op = isng ? pcap_ng_next_block : pcap_ng_next_packet;
	
	/*
	 * Special using block based API
	 */
	if (isng) {
		/*
		 * Rewind to begining of Section Header Block
		 */
		if (file_offset < 4) {
			snprintf(errbuf, PCAP_ERRBUF_SIZE, "bad file offset");
			goto fail;
		}
		file_offset -= 4;
		fseek(fp, file_offset, SEEK_SET);
		
		p->linktype = DLT_PCAPNG;
	}
	
	return (1);
	
fail:
	free(p->buffer);
	return (-1);
}

/*
 * The block is in p->buffer
 * We leave the content of the block intact and do not attempt to 
 * correct the byte order as this will be done by the caller.
 */
static int
pcap_ng_next_block(pcap_t *p, struct pcap_pkthdr *hdr, u_char **data)
{
	struct block_cursor cursor;
	int status;
	struct pcapng_enhanced_packet_fields *epbp;
	struct pcapng_simple_packet_fields *spbp;
	struct pcapng_packet_fields *pbp;
	struct pcapng_interface_description_fields *idbp;
	struct pcapng_section_header_fields *shbp;
	FILE *fp = p->sf.rfile;
	u_int tsresol;
	u_int64_t tsoffset;
	u_int64_t t, sec, frac;
	u_short interface_id = 0xFFFF;
	unsigned char packetpad;
	
	/*
	 * Read the block type and length; those are common
	 * to all blocks.
	 */
	status = read_block(fp, p, &cursor, p->errbuf);
	if (status == 0)
		return (1);	/* EOF */
	if (status == -1)
		return (-1);	/* error */
	
	memset(hdr, 0, sizeof(struct pcap_pkthdr));
	
	switch (cursor.block_type) {
			
		case PCAPNG_BT_EPB:
			/*
			 * Get a pointer to the fixed-length portion of the
			 * EPB.
			 */
			epbp = get_from_block_data(&cursor, sizeof(*epbp),
									   p->errbuf);
			if (epbp == NULL)
				return (-1);	/* error */
			
			/*
			 * Byte-swap it if necessary.
			 */
			if (p->sf.swapped) {
				interface_id = SWAPLONG(epbp->interface_id);
				hdr->caplen = SWAPLONG(epbp->caplen);
				hdr->len = SWAPLONG(epbp->len);
				t = ((u_int64_t)SWAPLONG(epbp->timestamp_high)) << 32 |
					SWAPLONG(epbp->timestamp_low);
			} else {
				interface_id = epbp->interface_id;
				hdr->caplen = epbp->caplen;
				hdr->len = epbp->len;
				t = ((u_int64_t)epbp->timestamp_high) << 32 |
					epbp->timestamp_low;
			}
			goto found_packet;
			
		case PCAPNG_BT_SPB:
			/*
			 * Get a pointer to the fixed-length portion of the
			 * SPB.
			 */
			spbp = get_from_block_data(&cursor, sizeof(*spbp),
									   p->errbuf);
			if (spbp == NULL)
				return (-1);	/* error */
			
			/*
			 * SPB packets are assumed to have arrived on
			 * the first interface.
			 */
			interface_id = 0;
			
			/*
			 * Byte-swap it if necessary.
			 */
			if (p->sf.swapped) {
				hdr->len = SWAPLONG(spbp->len);
			} else {
				hdr->len = spbp->len;
			}
			/*
			 * The SPB doesn't give the captured length;
			 * it's the minimum of the snapshot length
			 * and the packet length.
			 */
			hdr->caplen = hdr->len;
			if (hdr->caplen > p->snapshot)
				hdr->caplen = p->snapshot;
			t = 0;	/* no time stamps */
			
			goto found_packet;
			
		case PCAPNG_BT_PB:
			/*
			 * Get a pointer to the fixed-length portion of the
			 * PB.
			 */
			pbp = get_from_block_data(&cursor, sizeof(*pbp),
									  p->errbuf);
			if (pbp == NULL)
				return (-1);	/* error */
			
			/*
			 * Byte-swap it if necessary.
			 */
			if (p->sf.swapped) {
				/* these were written in opposite byte order */
				interface_id = SWAPSHORT(pbp->interface_id);
				hdr->caplen = SWAPLONG(pbp->caplen);
				hdr->len = SWAPLONG(pbp->len);
				t = ((u_int64_t)SWAPLONG(epbp->timestamp_high)) << 32 |
				SWAPLONG(epbp->timestamp_low);
			} else {
				interface_id = pbp->interface_id;
				hdr->caplen = pbp->caplen;
				hdr->len = pbp->len;
				t = ((u_int64_t)pbp->timestamp_high) << 32 |
					pbp->timestamp_low;
			}
			goto found_packet;
			
		case PCAPNG_BT_IDB:
			
			/*
			 * Interface Description Block.  Get a pointer
			 * to its fixed-length portion.
			 */
			idbp = get_from_block_data(&cursor, sizeof(*idbp),
									   p->errbuf);
			if (idbp == NULL)
				return (-1);	/* error */
			
			/*
			 * Set the default time stamp resolution and offset.
			 */
			tsresol = 1000000;	/* microsecond resolution */
			tsoffset = 0;		/* absolute timestamps */
			
			/*
			 * Now look for various time stamp options, to
			 * make sure they're the same.
			 *
			 * XXX - we could, in theory, handle multiple
			 * different resolutions and offsets, but we
			 * don't do so for now.
			 */
			if (process_idb_options(p, idbp, &cursor, &tsresol, &tsoffset,
									p->errbuf) == -1)
				return (-1);
			if (tsresol != p->sf.tsresol) {
				snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
						 "an interface has a time stamp resolution different from the time stamp resolution of the first interface");
				return (-1);
			}
			if (tsoffset != p->sf.tsoffset) {
				snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
						 "an interface has a time stamp offset different from the time stamp offset of the first interface");
				return (-1);
			}
			break;
		
		case PCAPNG_BT_SHB: {
			bpf_u_int32	byte_order_magic;
			u_short		major_version;

			/*
			 * Section Header Block.  Get a pointer
			 * to its fixed-length portion.
			 */
			shbp = get_from_block_data(&cursor, sizeof(*shbp),
									   p->errbuf);
			if (shbp == NULL)
				return (-1);	/* error */
			
			/*
			 * Assume the byte order of this section is
			 * the same as that of the previous section.
			 * We'll check for that later.
			 */
			if (p->sf.swapped) {
				byte_order_magic = SWAPLONG(shbp->byte_order_magic);
				major_version = SWAPSHORT(shbp->major_version);
			} else {
				byte_order_magic = shbp->byte_order_magic;
				major_version = shbp->major_version;
			}
			
			/*
			 * Make sure the byte order doesn't change;
			 * pcap_is_swapped() shouldn't change its
			 * return value in the middle of reading a capture.
			 */
			switch (byte_order_magic) {
					
				case PCAPNG_BYTE_ORDER_MAGIC:
					/*
					 * OK.
					 */
					break;
					
				case SWAPLONG(PCAPNG_BYTE_ORDER_MAGIC):
					/*
					 * Byte order changes.
					 */
					snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
							 "the file has sections with different byte orders");
					return (-1);
					
				default:
					/*
					 * Not a valid SHB.
					 */
					snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
							 "the file has a section with a bad byte order magic field");
					return (-1);
			}
			
			/*
			 * Make sure the major version is the version
			 * we handle.
			 */
			if (major_version != PCAPNG_VERSION_MAJOR) {
				snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
						 "unknown pcap-ng savefile major version number %u",
						 major_version);
				return (-1);
			}
			
			/*
			 * Reset the interface count; this section should
			 * have its own set of IDBs.  If any of them
			 * don't have the same interface type, snapshot
			 * length, or resolution as the first interface
			 * we saw, we'll fail.  (And if we don't see
			 * any IDBs, we'll fail when we see a packet
			 * block.)
			 */
			p->ifcount = 0;
			break;
		}
		default:
			/*
			 * Not a packet block, IDB, or SHB; ignore it.
			 */
			break;
	}
	goto done;
	
found_packet:
	/*
	 * Is the interface ID an interface we know?
	 */
	if (interface_id >= p->ifcount) {
		/*
		 * Yes.  Fail.
		 */
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
				 "a packet arrived on interface %u, but there's no Interface Description Block for that interface",
				 interface_id);
		return (-1);
	}
	
	/*
	 * Convert the time stamp to a struct timeval.
	 */
	sec = t / p->sf.tsresol + p->sf.tsoffset;
	frac = t % p->sf.tsresol;
	if (p->sf.tsresol > 1000000) {
		/*
		 * Higher than microsecond resolution; scale down to
		 * microseconds.
		 */
		frac /= p->sf.tsscale;
	} else {
		/*
		 * Lower than microsecond resolution; scale up to
		 * microseconds.
		 */
		frac *= p->sf.tsscale;
	}
	hdr->ts.tv_sec = sec;
	hdr->ts.tv_usec = frac;
	
	/*
	 * Get a pointer to the packet data.
	 */
	*data = get_from_block_data(&cursor, hdr->caplen, p->errbuf);
	if (*data == NULL)
		return (-1);
	
	/*
	 * Skip padding.
	 */
	packetpad = 4 - (hdr->caplen % 4);
	if (hdr->caplen % 4 != 0 &&
		get_from_block_data(&cursor, packetpad, NULL) == NULL)
		return (-1);
		
done:
	
	return (0);
}

/*
 * Read and return the next packet from the savefile.  Return the header
 * in hdr and a pointer to the contents in data.  Return 0 on success, 1
 * if there were no more packets, and -1 on an error.
 */
static int
pcap_ng_next_packet(pcap_t *p, struct pcap_pkthdr *hdr, u_char **data)
{
	struct block_cursor cursor;
	int status;
	struct pcapng_enhanced_packet_fields *epbp;
	struct pcapng_simple_packet_fields *spbp;
	struct pcapng_packet_fields *pbp;
	struct pcapng_option_header opthdr;
	bpf_u_int32 interface_id = 0xFFFFFFFF;
	struct pcapng_interface_description_fields *idbp;
	struct pcapng_section_header_fields *shbp;
	FILE *fp = p->sf.rfile;
	u_int tsresol;
	u_int64_t tsoffset;
	u_int64_t t, sec, frac;
	unsigned char packetpad;
	
	/*
	 * Look for an Enhanced Packet Block, a Simple Packet Block,
	 * or a Packet Block.
	 */
	for (;;) {
		/*
		 * Read the block type and length; those are common
		 * to all blocks.
		 */
		status = read_block(fp, p, &cursor, p->errbuf);
		if (status == 0)
			return (1);	/* EOF */
		if (status == -1)
			return (-1);	/* error */
		switch (cursor.block_type) {
				
			case PCAPNG_BT_EPB:
				/*
				 * Get a pointer to the fixed-length portion of the
				 * EPB.
				 */
				epbp = get_from_block_data(&cursor, sizeof(*epbp),
										   p->errbuf);
				if (epbp == NULL)
					return (-1);	/* error */
				
				/*
				 * Byte-swap it if necessary.
				 */
				if (p->sf.swapped) {
					/* these were written in opposite byte order */
					interface_id = SWAPLONG(epbp->interface_id);
					hdr->caplen = SWAPLONG(epbp->caplen);
					hdr->len = SWAPLONG(epbp->len);
					t = ((u_int64_t)SWAPLONG(epbp->timestamp_high)) << 32 |
					SWAPLONG(epbp->timestamp_low);
				} else {
					interface_id = epbp->interface_id;
					hdr->caplen = epbp->caplen;
					hdr->len = epbp->len;
					t = ((u_int64_t)epbp->timestamp_high) << 32 |
					epbp->timestamp_low;
				}
				goto found;
				
			case PCAPNG_BT_SPB:
				/*
				 * Get a pointer to the fixed-length portion of the
				 * SPB.
				 */
				spbp = get_from_block_data(&cursor, sizeof(*spbp),
										   p->errbuf);
				if (spbp == NULL)
					return (-1);	/* error */
				
				/*
				 * SPB packets are assumed to have arrived on
				 * the first interface.
				 */
				interface_id = 0;
				
				/*
				 * Byte-swap it if necessary.
				 */
				if (p->sf.swapped) {
					/* these were written in opposite byte order */
					hdr->len = SWAPLONG(spbp->len);
				} else
					hdr->len = spbp->len;
				
				/*
				 * The SPB doesn't give the captured length;
				 * it's the minimum of the snapshot length
				 * and the packet length.
				 */
				hdr->caplen = hdr->len;
				if (hdr->caplen > p->snapshot)
					hdr->caplen = p->snapshot;
				t = 0;	/* no time stamps */
				goto found;
				
			case PCAPNG_BT_PB:
				/*
				 * Get a pointer to the fixed-length portion of the
				 * PB.
				 */
				pbp = get_from_block_data(&cursor, sizeof(*pbp),
										  p->errbuf);
				if (pbp == NULL)
					return (-1);	/* error */
				
				/*
				 * Byte-swap it if necessary.
				 */
				if (p->sf.swapped) {
					/* these were written in opposite byte order */
					interface_id = SWAPSHORT(pbp->interface_id);
					hdr->caplen = SWAPLONG(pbp->caplen);
					hdr->len = SWAPLONG(pbp->len);
					t = ((u_int64_t)SWAPLONG(pbp->timestamp_high)) << 32 |
					SWAPLONG(pbp->timestamp_low);
				} else {
					interface_id = pbp->interface_id;
					hdr->caplen = pbp->caplen;
					hdr->len = pbp->len;
					t = ((u_int64_t)pbp->timestamp_high) << 32 |
					pbp->timestamp_low;
				}
				goto found;
				
			case PCAPNG_BT_IDB:
				/*
				 * Interface Description Block.  Get a pointer
				 * to its fixed-length portion.
				 */
				idbp = get_from_block_data(&cursor, sizeof(*idbp),
										   p->errbuf);
				if (idbp == NULL)
					return (-1);	/* error */
				
				/*
				 * Byte-swap it if necessary.
				 */
				if (p->sf.swapped) {
					idbp->linktype = SWAPSHORT(idbp->linktype);
					idbp->snaplen = SWAPLONG(idbp->snaplen);
				}
				
				/*
				 * If the link-layer type or snapshot length
				 * differ from the ones for the first IDB we
				 * saw, quit.
				 *
				 * XXX - just discard packets from those
				 * interfaces?
				 */
				if (p->linktype != idbp->linktype) {
					snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
							 "an interface has a type %u different from the type of the first interface",
							 idbp->linktype);
					return (-1);
				}
				if (p->snapshot != idbp->snaplen) {
					snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
							 "an interface has a snapshot length %u different from the type of the first interface",
							 idbp->snaplen);
					return (-1);
				}
				
				/*
				 * Set the default time stamp resolution and offset.
				 */
				tsresol = 1000000;	/* microsecond resolution */
				tsoffset = 0;		/* absolute timestamps */
				
				/*
				 * Now look for various time stamp options, to
				 * make sure they're the same.
				 *
				 * XXX - we could, in theory, handle multiple
				 * different resolutions and offsets, but we
				 * don't do so for now.
				 */
				if (process_idb_options(p, idbp, &cursor, &tsresol, &tsoffset,
										p->errbuf) == -1)
					return (-1);
				if (tsresol != p->sf.tsresol) {
					snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
							 "an interface has a time stamp resolution different from the time stamp resolution of the first interface");
					return (-1);
				}
				if (tsoffset != p->sf.tsoffset) {
					snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
							 "an interface has a time stamp offset different from the time stamp offset of the first interface");
					return (-1);
				}
				break;
				
			case PCAPNG_BT_SHB:
				/*
				 * Section Header Block.  Get a pointer
				 * to its fixed-length portion.
				 */
				shbp = get_from_block_data(&cursor, sizeof(*shbp),
										   p->errbuf);
				if (shbp == NULL)
					return (-1);	/* error */
				
				/*
				 * Assume the byte order of this section is
				 * the same as that of the previous section.
				 * We'll check for that later.
				 */
				if (p->sf.swapped) {
					shbp->byte_order_magic =
					SWAPLONG(shbp->byte_order_magic);
					shbp->major_version =
					SWAPSHORT(shbp->major_version);
				}
				
				/*
				 * Make sure the byte order doesn't change;
				 * pcap_is_swapped() shouldn't change its
				 * return value in the middle of reading a capture.
				 */
				switch (shbp->byte_order_magic) {
						
					case PCAPNG_BYTE_ORDER_MAGIC:
						/*
						 * OK.
						 */
						break;
						
					case SWAPLONG(PCAPNG_BYTE_ORDER_MAGIC):
						/*
						 * Byte order changes.
						 */
						snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
								 "the file has sections with different byte orders");
						return (-1);
						
					default:
						/*
						 * Not a valid SHB.
						 */
						snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
								 "the file has a section with a bad byte order magic field");
						return (-1);
				}
				
				/*
				 * Make sure the major version is the version
				 * we handle.
				 */
				if (shbp->major_version != PCAPNG_VERSION_MAJOR) {
					snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
							 "unknown pcap-ng savefile major version number %u",
							 shbp->major_version);
					return (-1);
				}
				
				/*
				 * Reset the interface count; this section should
				 * have its own set of IDBs.  If any of them
				 * don't have the same interface type, snapshot
				 * length, or resolution as the first interface
				 * we saw, we'll fail.  (And if we don't see
				 * any IDBs, we'll fail when we see a packet
				 * block.)
				 */
				p->ifcount = 0;
				break;
				
			default:
				/*
				 * Not a packet block, IDB, or SHB; ignore it.
				 */
				break;
		}
	}
	
found:
	/*
	 * Is the interface ID an interface we know?
	 */
	if (interface_id >= p->ifcount) {
		/*
		 * Yes.  Fail.
		 */
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
				 "a packet arrived on interface %u, but there's no Interface Description Block for that interface",
				 interface_id);
		return (-1);
	}
	
	/*
	 * Convert the time stamp to a struct timeval.
	 */
	sec = t / p->sf.tsresol + p->sf.tsoffset;
	frac = t % p->sf.tsresol;
	if (p->sf.tsresol > 1000000) {
		/*
		 * Higher than microsecond resolution; scale down to
		 * microseconds.
		 */
		frac /= p->sf.tsscale;
	} else {
		/*
		 * Lower than microsecond resolution; scale up to
		 * microseconds.
		 */
		frac *= p->sf.tsscale;
	}
	hdr->ts.tv_sec = sec;
	hdr->ts.tv_usec = frac;
	
	/*
	 * Get a pointer to the packet data.
	 */
	*data = get_from_block_data(&cursor, hdr->caplen, p->errbuf);
	if (*data == NULL)
		return (-1);
	
	/*
	 * Skip padding.
	 */
	packetpad = 4 - (hdr->caplen % 4);
	if (hdr->caplen % 4 != 0 &&
		get_from_block_data(&cursor, packetpad, NULL) == NULL)
		return (-1);
	
	memset(hdr->comment, 0, sizeof(hdr->comment));

	if (get_opthdr_from_block_data(&opthdr, p->sf.swapped, &cursor, NULL) != NULL &&
		opthdr.option_code == PCAPNG_OPT_COMMENT && opthdr.option_length > 0) {
		char *optvalue;
		optvalue = get_optvalue_from_block_data(&cursor, &opthdr, NULL);
		if (optvalue == NULL)
			return (-1);
		memcpy(hdr->comment, optvalue, sizeof(hdr->comment));
	}
	
	if (p->sf.swapped) {
		/*
		 * Convert pseudo-headers from the byte order of
		 * the host on which the file was saved to our
		 * byte order, as necessary.
		 */
		switch (p->linktype) {
				
			case DLT_USB_LINUX:
				swap_linux_usb_header(hdr, *data, 0);
				break;
				
			case DLT_USB_LINUX_MMAPPED:
				swap_linux_usb_header(hdr, *data, 1);
				break;
		}
	}
	
	return (0);
}

static int
sf_ng_write_header(FILE *fp, int linktype, int thiszone, int snaplen)
{
	struct pcapng_block_header bh;
	struct pcapng_section_header_fields shb;
	struct pcapng_interface_description_fields idb;
	struct pcapng_block_trailer bt;
	size_t len;
	
	/*
	 * Section Header Block
	 */
	len = sizeof(bh) + sizeof(shb) + sizeof(bt);
	bh.block_type   = PCAPNG_BT_SHB;
	bh.total_length = len;
	
	shb.byte_order_magic = PCAPNG_BYTE_ORDER_MAGIC;
	shb.major_version    = PCAPNG_VERSION_MAJOR;
	shb.minor_version    = 0;
	shb.section_length   = 0xFFFFFFFFFFFFFFFF;
	
	bt.total_length = len;
	
	if (fwrite((char *)&bh, sizeof(bh), 1, fp) != 1)
		return (-1);
	
	if (fwrite((char *)&shb, sizeof(shb), 1, fp) != 1)
		return (-1);
	
	if (fwrite((char *)&bt, sizeof(bt), 1, fp) != 1)
		return (-1);
	
	/*
	 * Interface Description Block
	 */
	len = sizeof(bh) + sizeof(idb) + sizeof(bt);
	bh.block_type   = PCAPNG_BT_IDB;
	bh.total_length = len;
	
	idb.reserved = 0;
	idb.linktype = linktype;
	idb.snaplen  = snaplen;
	
	bt.total_length = len;
	
	if (fwrite((char *)&bh, sizeof(bh), 1, fp) != 1)
		return (-1);
	
	if (fwrite((char *)&idb, sizeof(idb), 1, fp) != 1)
		return (-1);
	
	if (fwrite((char *)&bt, sizeof(bt), 1, fp) != 1)
		return (-1);
	
	return (0);
}

static pcap_dumper_t *
pcap_ng_setup_dump(pcap_t *p, int linktype, FILE *f, const char *fname)
{
	if (sf_ng_write_header(f, linktype, p->tzoff, p->snapshot) == -1) {
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "Can't write to %s: %s",
				 fname, pcap_strerror(errno));
		if (f != stdout)
			(void)fclose(f);
		return (NULL);
	}
	p->shb_added = 1;
	return ((pcap_dumper_t *)f);
}

pcap_dumper_t *
pcap_ng_dump_open(pcap_t *p, const char *fname)
{
	FILE *f;
	int linktype;
	
	/*
	 * If this pcap_t hasn't been activated, it doesn't have a
	 * link-layer type, so we can't use it.
	 */
	if (!p->activated) {
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
				 "%s: not-yet-activated pcap_t passed to pcap_ng_dump_open",
				 fname);
		return (NULL);
	}
	
	if (fname[0] == '-' && fname[1] == '\0') {
		f = stdout;
		fname = "standard output";
	} else {
		f = fopen(fname, "wb");
		if (f == NULL) {
			snprintf(p->errbuf, PCAP_ERRBUF_SIZE, "%s: %s",
					 fname, pcap_strerror(errno));
			return (NULL);
		}
	}
	
	/*
	 * Make sure a section header will be added and that
	 * any information to a previous section gets cleared.
	 */
	pcap_ng_init_section_info(p);
	
	/*
	 * When using the block based API, the section header and 
	 * interface description blocks are given by the caller
	 */
	if (p->linktype != DLT_PKTAP && p->linktype != DLT_PCAPNG) {
		linktype = dlt_to_linktype(p->linktype);
		if (linktype == -1) {
			snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
					 "%s: link-layer type %d isn't supported in savefiles",
					 fname, p->linktype);
			if (f != stdout)
				fclose(f);
			return (NULL);
		}
		linktype |= p->linktype_ext;
		
		return (pcap_ng_setup_dump(p, linktype, f, fname));
	} else {
		return ((pcap_dumper_t *)f);
	}
}

pcap_dumper_t *
pcap_ng_dump_fopen(pcap_t *p, FILE *f)
{
	int linktype;
	
	linktype = dlt_to_linktype(p->linktype);
	if (linktype == -1) {
		snprintf(p->errbuf, PCAP_ERRBUF_SIZE,
				 "stream: link-layer type %d isn't supported in savefiles",
				 p->linktype);
		return (NULL);
	}
	linktype |= p->linktype_ext;
	
	return (pcap_ng_setup_dump(p, linktype, f, "stream"));	
}

void
pcap_ng_dump(u_char *user, const struct pcap_pkthdr *h, const u_char *sp)
{
	FILE *f;
	uint64_t ts;
	struct pcapng_block_header bh;
	struct pcapng_enhanced_packet_fields epb;
	struct pcapng_block_trailer bt;
	unsigned char packetpad = 0;
	size_t len = sizeof(bh) + sizeof(epb) + h->caplen + sizeof(bt);
	struct pcapng_option_header ohcomm;
	struct pcapng_option_header oht;
	size_t commlen = 0;
	unsigned char commpad = 0;
	
	if (h->comment[0]) {
		/* Comment option */
		commlen = strlen(h->comment);
		ohcomm.option_code   = PCAPNG_OPT_COMMENT;
		ohcomm.option_length = commlen;
		len += sizeof(ohcomm) + commlen;
		if (commlen % 4 != 0) {
			commpad = 4 - (commlen % 4);
			len += commpad;
		}
		/* Option terminator */
		oht.option_code      = PCAPNG_OPT_ENDOFOPT;
		oht.option_length    = 0;
		len += sizeof(oht);
	}
	
	if (h->caplen % 4 != 0) {
		packetpad = 4 - (h->caplen % 4);
		len += packetpad;
	}
	
	bh.block_type   = PCAPNG_BT_EPB;
	bh.total_length = len;
	
	epb.caplen		   = h->caplen;
	epb.interface_id   = 0;
	epb.len            = h->len;
	/* Microsecond resolution */
	ts = h->ts.tv_sec * 1000000 + h->ts.tv_usec;
	epb.timestamp_high = ts >> 32;
	epb.timestamp_low  = ts & 0xffffffff;
	
	bt.total_length = len;
	
	f = (FILE *)user;
	/* XXX we should check the return status */
	/* Header */
	(void)fwrite(&bh, sizeof(bh), 1, f);
	(void)fwrite(&epb, sizeof(epb), 1, f);
	/* Packet */
	(void)fwrite(sp, h->caplen, 1, f);
	if (packetpad)
		(void)fseek(f, packetpad, SEEK_CUR);
	/* Options */
	if (h->comment[0]) {
		(void)fwrite(&ohcomm, sizeof(ohcomm), 1, f);
		(void)fwrite(h->comment, commlen, 1, f);
		if (commpad)
			(void)fseek(f, commpad, SEEK_CUR);
		(void)fwrite(&oht, sizeof(oht), 1, f);
	}
	(void)fwrite(&bt, sizeof(bt), 1, f);
}

void
pcap_ng_dump_close(pcap_dumper_t *p)
{
	/*
	 * XXX we could add an interface statistics block at the end
	 * of the file.
	 */
	return pcap_dump_close(p);
}
