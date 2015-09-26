/*
 * This file is part of dmrshark.
 *
 * dmrshark is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dmrshark is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dmrshark.  If not, see <http://www.gnu.org/licenses/>.
**/

#include DEFAULTCONFIG

#include "repeaters.h"
#include "comm.h"
#include "snmp.h"
#include "ipsc.h"

#include <libs/daemon/console.h>
#include <libs/daemon/daemon-poll.h>
#include <libs/config/config.h>
#include <libs/remotedb/remotedb.h>
#include <libs/base/dmr-handle.h>
#include <libs/base/base.h>
#include <libs/dmrpacket/dmrpacket-emb.h>
#include <libs/dmrpacket/dmrpacket-lc.h>
#include <libs/voicestreams/voicestreams-decode.h>
#include <libs/coding/crc.h>

#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

static repeater_t *repeaters = NULL;

static char *repeaters_get_readable_slot_state(repeater_slot_state_t state) {
	switch (state) {
		case REPEATER_SLOT_STATE_IDLE: return "idle";
		case REPEATER_SLOT_STATE_CALL_RUNNING: return "call running";
		case REPEATER_SLOT_STATE_DATA_RECEIVE_RUNNING: return "data receive running";
		default: return "unknown";
	}
}

char *repeaters_get_display_string_for_ip(struct in_addr *ipaddr) {
	repeater_t *foundrep;

	foundrep = repeaters_findbyip(ipaddr);
	if (foundrep && foundrep->callsign_lowercase[0] != 0)
		return foundrep->callsign_lowercase;
	if (comm_is_our_ipaddr(ipaddr))
		return "ds";

	return comm_get_ip_str(ipaddr);
}

char *repeaters_get_display_string(repeater_t *repeater) {
	if (repeater->callsign[0] == 0)
		return comm_get_ip_str(&repeater->ipaddr);
	else
		return repeater->callsign_lowercase;
}

repeater_t *repeaters_findbyip(struct in_addr *ipaddr) {
	repeater_t *repeater = repeaters;

	if (ipaddr == NULL)
		return NULL;

	while (repeater) {
		if (memcmp(&repeater->ipaddr, ipaddr, sizeof(struct in_addr)) == 0)
			return repeater;

		repeater = repeater->next;
	}
	return NULL;
}

repeater_t *repeaters_findbyhost(char *host) {
	struct in_addr ipaddr;

	if (comm_hostname_to_ip(host, &ipaddr))
		return repeaters_findbyip(&ipaddr);
	else
		return NULL;
}

repeater_t *repeaters_findbycallsign(char *callsign) {
	repeater_t *repeater = repeaters;

	if (callsign == NULL)
		return NULL;

	while (repeater) {
		if (strcasecmp(repeater->callsign, callsign) == 0)
			return repeater;

		repeater = repeater->next;
	}
	return NULL;
}

repeater_t *repeaters_get_active(dmr_id_t src_id, dmr_id_t dst_id, dmr_call_type_t call_type) {
	repeater_t *repeater = repeaters;

	while (repeater) {
		if ((repeater->slot[0].state != REPEATER_SLOT_STATE_IDLE && repeater->slot[0].src_id == src_id && repeater->slot[0].dst_id == dst_id && repeater->slot[0].call_type == call_type) ||
			(repeater->slot[1].state != REPEATER_SLOT_STATE_IDLE && repeater->slot[1].src_id == src_id && repeater->slot[1].dst_id == dst_id && repeater->slot[1].call_type == call_type))
				return repeater;

		repeater = repeater->next;
	}
	return NULL;
}

static flag_t repeaters_issnmpignoredforip(struct in_addr *ipaddr) {
	char *ignoredhosts = config_get_ignoredsnmprepeaterhosts();
	char *tok = NULL;
	struct in_addr ignoredaddr;

	tok = strtok(ignoredhosts, ",");
	if (tok) {
		do {
			if (comm_hostname_to_ip(tok, &ignoredaddr)) {
				if (memcmp(&ignoredaddr, ipaddr, sizeof(struct in_addr)) == 0) {
					free(ignoredhosts);
					return 1;
				}
			} else
				console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "repeaters: can't resolve hostname %s\n", tok);

			tok = strtok(NULL, ",");
		} while (tok != NULL);
	}
	free(ignoredhosts);
	return 0;
}

static void repeaters_remove(repeater_t *repeater) {
	ipscrawpacketbuf_t *pb_nextentry;

	if (repeater == NULL)
		return;

	console_log("repeaters [%s]: removing\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));

	vbptc_16_11_free(&repeater->slot[0].emb_sig_lc_vbptc_storage);
	vbptc_16_11_free(&repeater->slot[1].emb_sig_lc_vbptc_storage);
	vbptc_16_11_free(&repeater->slot[0].ipsc_tx_emb_sig_lc_vbptc_storage);
	vbptc_16_11_free(&repeater->slot[1].ipsc_tx_emb_sig_lc_vbptc_storage);

	repeaters_free_echo_buf(repeater, 0);
	repeaters_free_echo_buf(repeater, 1);

	// Freeing up IPSC packet buffers for both slots.
	while (repeater->slot[0].ipsc_tx_rawpacketbuf) {
		pb_nextentry = repeater->slot[0].ipsc_tx_rawpacketbuf->next;
		free(repeater->slot[0].ipsc_tx_rawpacketbuf);
		repeater->slot[0].ipsc_tx_rawpacketbuf = pb_nextentry;
	}
	while (repeater->slot[1].ipsc_tx_rawpacketbuf) {
		pb_nextentry = repeater->slot[1].ipsc_tx_rawpacketbuf->next;
		free(repeater->slot[1].ipsc_tx_rawpacketbuf);
		repeater->slot[1].ipsc_tx_rawpacketbuf = pb_nextentry;
	}

	if (repeater->prev)
		repeater->prev->next = repeater->next;
	if (repeater->next)
		repeater->next->prev = repeater->prev;

	if (repeater == repeaters)
		repeaters = repeater->next;

	free(repeater);
}

repeater_t *repeaters_add(struct in_addr *ipaddr) {
	flag_t error = 0;
	repeater_t *repeater = repeaters_findbyip(ipaddr);

	if (ipaddr == NULL)
		return NULL;

	if (repeater == NULL) {
		repeater = (repeater_t *)calloc(sizeof(repeater_t), 1);
		if (repeater == NULL) {
			console_log("repeaters [%s]: can't add new repeater, not enough memory\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
			return NULL;
		}
		memcpy(&repeater->ipaddr, ipaddr, sizeof(struct in_addr));

		// Expecting 8 rows of variable length BPTC coded embedded LC data.
		// It will contain 77 data bits (without the Hamming (16,11) checksums
		// and the last row of parity bits).
		if (!vbptc_16_11_init(&repeater->slot[0].emb_sig_lc_vbptc_storage, 8))
			error = 1;
		else {
			if (!vbptc_16_11_init(&repeater->slot[1].emb_sig_lc_vbptc_storage, 8)) {
				vbptc_16_11_free(&repeater->slot[0].emb_sig_lc_vbptc_storage);
				error = 1;
			}
		}
		if (error) {
			console_log("repeaters [%s]: can't add, not enough memory for embedded signalling lc storage\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
			free(repeater);
			return NULL;
		}

		if (repeaters_issnmpignoredforip(ipaddr))
			repeater->snmpignored = 1;

		repeater->slot[0].voicestream = voicestreams_get_stream_for_repeater(ipaddr, 1);
#ifdef AMBEDECODEVOICE
		voicestreams_decode_ambe_init(repeater->slot[0].voicestream);
#endif
		repeater->slot[1].voicestream = voicestreams_get_stream_for_repeater(ipaddr, 2);
#ifdef AMBEDECODEVOICE
		voicestreams_decode_ambe_init(repeater->slot[1].voicestream);
#endif
		if (repeaters != NULL) {
			repeaters->prev = repeater;
			repeater->next = repeaters;
		}
		repeaters = repeater;

		console_log("repeaters [%s]: added, snmp ignored: %u ts1 stream: %s ts2 stream: %s\n",
			repeaters_get_display_string_for_ip(&repeater->ipaddr), repeater->snmpignored,
			repeater->slot[0].voicestream != NULL ? repeater->slot[0].voicestream->name : "no stream defined",
			repeater->slot[1].voicestream != NULL ? repeater->slot[1].voicestream->name : "no stream defined");
	}
	repeater->last_active_time = time(NULL);

	return repeater;
}

void repeaters_list(void) {
	repeater_t *repeater = repeaters;
	int i = 1;

	if (repeaters == NULL) {
		console_log("no repeaters found yet\n");
		return;
	}

	console_log("repeaters:\n");
	console_log("      nr              ip     id  callsign  act  lstinf         type        fwver    dlfreq    ulfreq snmp ts1/ts2 streams\n");
	while (repeater) {
		console_log("  #%4u: %15s %6u %9s %4u  %6u %12s %12s %9u %9u    %u %s / %s\n",
			i++,
			comm_get_ip_str(&repeater->ipaddr),
			repeater->id,
			repeater->callsign,
			time(NULL)-repeater->last_active_time,
			time(NULL)-repeater->last_repeaterinfo_request_time,
			repeater->type,
			repeater->fwversion,
			repeater->dlfreq,
			repeater->ulfreq,
			!repeater->snmpignored,
			repeater->slot[0].voicestream != NULL ? repeater->slot[0].voicestream->name : "n/a",
			repeater->slot[1].voicestream != NULL ? repeater->slot[1].voicestream->name : "n/a");

		repeater = repeater->next;
	}
}

void repeaters_state_change(repeater_t *repeater, dmr_timeslot_t timeslot, repeater_slot_state_t new_state) {
	console_log(LOGLEVEL_REPEATERS "repeaters [%s]: slot %u state change from %s to %s\n",
		repeaters_get_display_string_for_ip(&repeater->ipaddr), timeslot+1, repeaters_get_readable_slot_state(repeater->slot[timeslot].state),
		repeaters_get_readable_slot_state(new_state));
	repeater->slot[timeslot].state = new_state;

	if (repeater->auto_rssi_update_enabled_at != 0 &&
		repeater->slot[0].state != REPEATER_SLOT_STATE_CALL_RUNNING &&
		repeater->slot[1].state != REPEATER_SLOT_STATE_CALL_RUNNING) {
			console_log(LOGLEVEL_REPEATERS "repeaters [%s]: stopping auto repeater status update\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
			repeater->auto_rssi_update_enabled_at = 0;
	}
}

static void repeaters_add_to_ipsc_packet_buffer(repeater_t *repeater, dmr_timeslot_t ts, ipscpacket_raw_t *ipscpacket_raw) {
	ipscrawpacketbuf_t *newpbentry;
	ipscrawpacketbuf_t *pbentry;

	if (repeater == NULL || ipscpacket_raw == NULL)
		return;

	console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "repeaters [%s]: adding entry to ts%u ipsc packet buffer\n", repeaters_get_display_string_for_ip(&repeater->ipaddr), ts+1);

	newpbentry = (ipscrawpacketbuf_t *)calloc(1, sizeof(ipscrawpacketbuf_t));
	if (newpbentry == NULL) {
		console_log(LOGLEVEL_REPEATERS "repeaters [%s] error: couldn't allocate memory for new ipsc packet buffer entry\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
		return;
	}

	memcpy(&newpbentry->ipscpacket_raw, ipscpacket_raw, sizeof(ipscpacket_raw_t));

	pbentry = repeater->slot[ts].ipsc_tx_rawpacketbuf;
	if (pbentry == NULL)
		repeater->slot[ts].ipsc_tx_rawpacketbuf = newpbentry;
	else {
		// Searching for the last element in the packet buffer.
		while (pbentry->next)
			pbentry = pbentry->next;
		pbentry->next = newpbentry;
	}

	daemon_poll_setmaxtimeout(0);
}

// Sends given raw IPSC packet to the given repeater.
static flag_t repeaters_send_raw_ipsc_packet(repeater_t *repeater, ipscpacket_raw_t *ipscpacket_raw) {
	struct sockaddr_in sin;
	int sockfd;

	if (repeater == NULL || ipscpacket_raw == NULL)
		return 0;

	// Need to use raw socket here, because if the master software is running,
	// we can't bind to the source port to set it in our UDP packet.
	if ((sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) == -1) {
		console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "repeaters [%s]: can't create raw socket for sending an udp packet\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
		return 0;
	}

	memset(&sin, 0, sizeof(struct sockaddr_in));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(62006);
	memcpy(&sin.sin_addr, &repeater->ipaddr, sizeof(struct in_addr));

	errno = 0;
	if (sendto(sockfd, ipscpacket_raw->bytes, sizeof(ipscpacket_raw_t), MSG_DONTWAIT, (struct sockaddr *)&sin, sizeof(struct sockaddr_in)) != sizeof(ipscpacket_raw_t)) {
		console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "repeaters [%s]: can't send udp packet: %s\n", repeaters_get_display_string_for_ip(&repeater->ipaddr), strerror(errno));
		close(sockfd);
		return 0;
	}
	close(sockfd);
	return 1;
}

void repeaters_start_voice_call(repeater_t *repeater, dmr_timeslot_t ts, dmr_call_type_t calltype, dmr_id_t dstid, dmr_id_t srcid) {
	dmrpacket_emb_signalling_lc_bits_t *emb_signalling_lc_bits;
	uint8_t i;

	if (repeater == NULL)
		return;

	repeater->slot[ts].ipsc_tx_seqnum = 0;
	repeater->slot[ts].ipsc_tx_voice_frame_num = 2;
	if (!vbptc_16_11_init(&repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage, 8)) {
		console_log("repeaters [%s] error: can't allocate memory for vbptc encoding\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
		return;
	}
	emb_signalling_lc_bits = dmrpacket_emb_signalling_lc_interleave(dmrpacket_lc_construct_emb_signalling_lc(calltype, dstid, srcid));
	vbptc_16_11_construct(&repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage, emb_signalling_lc_bits->bits, sizeof(dmrpacket_emb_signalling_lc_bits_t));

	for (i = 0; i < 4; i++)
		repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_VOICE_LC_HEADER, calltype, dstid, srcid, ipscpacket_construct_payload_voice_lc_header(calltype, dstid, srcid))));
}

void repeaters_play_ambe_data(dmrpacket_payload_voice_bytes_t *voice_bytes, repeater_t *repeater, dmr_timeslot_t ts, dmr_call_type_t calltype, dmr_id_t dstid, dmr_id_t srcid) {
	dmrpacket_payload_voice_bits_t voice_bits;

	if (repeater == NULL || voice_bytes == NULL)
		return;

	base_bytestobits(voice_bytes->bytes, sizeof(dmrpacket_payload_voice_bytes_t), voice_bits.raw.bits, sizeof(dmrpacket_payload_voice_bits_t));

	switch (repeater->slot[ts].ipsc_tx_voice_frame_num) {
		case 0:
			repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_VOICE_DATA_A, calltype, dstid, srcid,
				ipscpacket_construct_payload_voice_frame(IPSCPACKET_SLOT_TYPE_VOICE_DATA_A, &voice_bits, &repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage))));
			break;
		case 1:
			repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_VOICE_DATA_B, calltype, dstid, srcid,
				ipscpacket_construct_payload_voice_frame(IPSCPACKET_SLOT_TYPE_VOICE_DATA_B, &voice_bits, &repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage))));
			break;
		case 2:
			repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_VOICE_DATA_C, calltype, dstid, srcid,
				ipscpacket_construct_payload_voice_frame(IPSCPACKET_SLOT_TYPE_VOICE_DATA_C, &voice_bits, &repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage))));
			break;
		case 3:
			repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_VOICE_DATA_D, calltype, dstid, srcid,
				ipscpacket_construct_payload_voice_frame(IPSCPACKET_SLOT_TYPE_VOICE_DATA_D, &voice_bits, &repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage))));
			break;
		case 4:
			repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_VOICE_DATA_E, calltype, dstid, srcid,
				ipscpacket_construct_payload_voice_frame(IPSCPACKET_SLOT_TYPE_VOICE_DATA_E, &voice_bits, &repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage))));
			break;
		case 5:
			repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_VOICE_DATA_F, calltype, dstid, srcid,
				ipscpacket_construct_payload_voice_frame(IPSCPACKET_SLOT_TYPE_VOICE_DATA_F, &voice_bits, &repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage))));
			break;
		default:
			break;
	}
	repeater->slot[ts].ipsc_tx_voice_frame_num++;
	if (repeater->slot[ts].ipsc_tx_voice_frame_num > 5)
		repeater->slot[ts].ipsc_tx_voice_frame_num = 0;
}

void repeaters_end_voice_call(repeater_t *repeater, dmr_timeslot_t ts, dmr_call_type_t calltype, dmr_id_t dstid, dmr_id_t srcid) {
	if (repeater == NULL)
		return;

	repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_TERMINATOR_WITH_LC, calltype, dstid, srcid, ipscpacket_construct_payload_terminator_with_lc(calltype, dstid, srcid))));
	vbptc_16_11_free(&repeater->slot[ts].ipsc_tx_emb_sig_lc_vbptc_storage);
}

void repeaters_play_ambe_file(char *ambe_file_name, repeater_t *repeater, dmr_timeslot_t ts, dmr_call_type_t calltype, dmr_id_t dstid, dmr_id_t srcid) {
	FILE *f;
	dmrpacket_payload_voice_bytes_t voice_bytes;

	if (ambe_file_name == NULL || repeater == NULL)
		return;

	f = fopen(ambe_file_name, "r");
	if (!f) {
		console_log("repeaters [%s] error: can't open %s for playing\n", repeaters_get_display_string_for_ip(&repeater->ipaddr), ambe_file_name);
		return;
	}

	console_log("repeaters [%s]: playing %s\n", repeaters_get_display_string_for_ip(&repeater->ipaddr), ambe_file_name);

	repeaters_start_voice_call(repeater, ts, calltype, dstid, srcid);
	while (!feof(f)) {
		if (fread(voice_bytes.bytes, 1, sizeof(dmrpacket_payload_voice_bytes_t), f) == sizeof(dmrpacket_payload_voice_bytes_t))
			repeaters_play_ambe_data(&voice_bytes, repeater, ts, calltype, dstid, srcid);
	}
	repeaters_end_voice_call(repeater, ts, calltype, dstid, srcid);
	fclose(f);

}

void repeaters_free_echo_buf(repeater_t *repeater, dmr_timeslot_t ts) {
	repeater_echo_buf_t *next_echo_buf;

	while (repeater->slot[ts].echo_buf_first_entry != NULL) {
		next_echo_buf = repeater->slot[ts].echo_buf_first_entry;
		free(repeater->slot[ts].echo_buf_first_entry);
		repeater->slot[ts].echo_buf_first_entry = next_echo_buf;
	}
	repeater->slot[ts].echo_buf_last_entry = NULL;
}

void repeaters_play_and_free_echo_buf(repeater_t *repeater, dmr_timeslot_t ts) {
	repeater_echo_buf_t *echo_buf;
	repeater_echo_buf_t *next_echo_buf;

	if (repeater == NULL || repeater->slot[ts].echo_buf_first_entry == NULL)
		return;

	// We need to use local variables here as processing outgoing IPSC packets could overwrite them.
	echo_buf = repeater->slot[ts].echo_buf_first_entry;
	repeater->slot[ts].echo_buf_first_entry = NULL;
	repeater->slot[ts].echo_buf_last_entry = NULL;

	repeaters_start_voice_call(repeater, ts, DMR_CALL_TYPE_GROUP, DMRSHARK_DEFAULT_DMR_ID, DMRSHARK_DEFAULT_DMR_ID);
	while (echo_buf != NULL) {
		repeaters_play_ambe_data(&echo_buf->voice_bytes, repeater, ts, DMR_CALL_TYPE_GROUP, DMRSHARK_DEFAULT_DMR_ID, DMRSHARK_DEFAULT_DMR_ID);

		next_echo_buf = echo_buf->next;
		free(echo_buf);
		echo_buf = next_echo_buf;
	}
	repeaters_end_voice_call(repeater, ts, DMR_CALL_TYPE_GROUP, DMRSHARK_DEFAULT_DMR_ID, DMRSHARK_DEFAULT_DMR_ID);
}

void repeaters_store_voice_frame_to_echo_buf(repeater_t *repeater, ipscpacket_t *ipscpacket) {
	repeater_echo_buf_t *new_echo_buf_entry;
	dmrpacket_payload_voice_bits_t *voice_bits;

	if (repeater == NULL || ipscpacket == NULL)
		return;

	new_echo_buf_entry = (repeater_echo_buf_t *)malloc(sizeof(repeater_echo_buf_t));
	if (new_echo_buf_entry == NULL) {
		console_log("  error: can't allocate memory for echo buffer\n");
		return;
	}

	console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "repeaters [%s]: storing ts%u voice frame to echo buf\n", repeaters_get_display_string_for_ip(&repeater->ipaddr),
		ipscpacket->timeslot);

	voice_bits = dmrpacket_extract_voice_bits(&ipscpacket->payload_bits);
	base_bitstobytes(voice_bits->raw.bits, sizeof(dmrpacket_payload_voice_bits_t), new_echo_buf_entry->voice_bytes.bytes, sizeof(dmrpacket_payload_voice_bits_t)/8);
	new_echo_buf_entry->next = NULL;

	if (repeater->slot[ipscpacket->timeslot-1].echo_buf_last_entry == NULL) {
		repeater->slot[ipscpacket->timeslot-1].echo_buf_last_entry = repeater->slot[ipscpacket->timeslot-1].echo_buf_first_entry = new_echo_buf_entry;
	} else {
		// Putting the new entry to the end of the linked list.
		repeater->slot[ipscpacket->timeslot-1].echo_buf_last_entry->next = new_echo_buf_entry;
		repeater->slot[ipscpacket->timeslot-1].echo_buf_last_entry = new_echo_buf_entry;
	}
}

void repeaters_send_sms(repeater_t *repeater, dmr_timeslot_t ts, dmr_call_type_t calltype, dmr_id_t dstid, dmr_id_t srcid, char *msg) {
	uint16_t i, j;
	dmrpacket_csbk_t csbk;
	dmrpacket_data_header_t data_header;
	ipscpacket_payload_t *ipscpacket_payload;
	dmrpacket_data_block_t *data_blocks;
	uint8_t data_blocks_needed;
	dmrpacket_data_fragment_t fragment;
	uint32_t fragment_crc = 0;
	uint16_t bytes_stored_in_blocks = 0;
	uint8_t bytes_to_store;
	uint8_t pad_octets = 0;
	uint8_t number_of_csbk_preambles_to_send = 10; // TODO
//TODO: szetszedni
//TODO: ack lekezeles, hogy ne legyen data timeout
//TODO: selective ack lekezeles
	if (repeater == NULL || msg == NULL)
		return;

	repeater->slot[ts].ipsc_tx_seqnum = 0;

	console_log("repeaters [%s]: sending %s sms to %u on ts%u: %s\n", repeaters_get_display_string_for_ip(&repeater->ipaddr), (calltype == DMR_CALL_TYPE_GROUP ? "group" : "private"), dstid, ts+1, msg);

	// Constructing the message fragment.
	memset(fragment.bytes, 0, DMRPACKET_MAX_FRAGMENTSIZE);
	fragment.bytes_stored = min(2+strlen(msg)*2, DMRPACKET_MAX_FRAGMENTSIZE);
	// Ignoring the first two bytes and placing empty bytes between each character.
	for (i = 2, j = 0; i < fragment.bytes_stored; i += 2, j++)
		fragment.bytes[i] = msg[j];

	// See DMR AI spec. page. 73. - confirmed rate 3/4
	data_blocks_needed = ceil(fragment.bytes_stored / 16.0);
	// Checking if there's no space left in the last data block for the fragment CRC.
	if (data_blocks_needed*16-fragment.bytes_stored < 4)
		data_blocks_needed++;

	pad_octets = (data_blocks_needed*16-4)-fragment.bytes_stored; // -4 - fragment CRC

	for (i = 0; i < fragment.bytes_stored+pad_octets; i += 2) {
		if (i+1 < fragment.bytes_stored)
			crc_calc_crc32(&fragment_crc, fragment.bytes[i+1]);
		else
			crc_calc_crc32(&fragment_crc, 0);
		if (i < fragment.bytes_stored)
			crc_calc_crc32(&fragment_crc, fragment.bytes[i]);
		else
			crc_calc_crc32(&fragment_crc, 0);
	}
	crc_calc_crc32_finish(&fragment_crc);

	console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "  message length: %u bytes, fragment crc: %.8x, needed blocks: %u, pad octets: %u\n",
		fragment.bytes_stored, fragment_crc, data_blocks_needed, pad_octets);
	console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "  message bytes: ");
	for (j = 0; j < fragment.bytes_stored; j++)
		console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "%.2x", fragment.bytes[j]);
	console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG " %.8x\n", fragment_crc);

	// Constructing message blocks from the fragment.
	data_blocks = (dmrpacket_data_block_t *)calloc(1, data_blocks_needed*sizeof(dmrpacket_data_block_t));
	if (data_blocks == NULL) {
		console_log("  error: can't allocate memory for data blocks\n");
		return;
	}

	for (i = 0; i < data_blocks_needed; i++) {
		data_blocks[i].serialnr = i;
		data_blocks[i].data_length = 16;

		if (i == data_blocks_needed-1) { // Storing the fragment CRC in the last block.
			data_blocks[i].data[data_blocks[i].data_length-1] = (fragment_crc >> 24) & 0xff;
			data_blocks[i].data[data_blocks[i].data_length-2] = (fragment_crc >> 16) & 0xff;
			data_blocks[i].data[data_blocks[i].data_length-3] = (fragment_crc >> 8) & 0xff;
			data_blocks[i].data[data_blocks[i].data_length-4] = fragment_crc & 0xff;
		}

		bytes_to_store = min(data_blocks[i].data_length, fragment.bytes_stored-bytes_stored_in_blocks);
		memcpy(data_blocks[i].data, fragment.bytes+bytes_stored_in_blocks, bytes_to_store);
		bytes_stored_in_blocks += bytes_to_store;

		data_blocks[i].crc = 0;
		for (j = 0; j < data_blocks[i].data_length; j++)
			crc_calc_crc9(&data_blocks[i].crc, data_blocks[i].data[j], 8);
		crc_calc_crc9(&data_blocks[i].crc, data_blocks[i].serialnr, 7);
		// Getting out only 8 bits from the shift registers as previously we only put in 7 bits.
		crc_calc_crc9_finish(&data_blocks[i].crc, 8);

		// Inverting according to the inversion polynomial.
		data_blocks[i].crc = ~data_blocks[i].crc;
		data_blocks[i].crc &= 0x01ff;
		// Applying CRC mask, see DMR AI spec. page 143.
		data_blocks[i].crc ^= 0x01ff;

		console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "  block #%u length: %u crc: %.4x bytes: ", i, data_blocks[i].data_length, data_blocks[i].crc);
		for (j = 0; j < data_blocks[i].data_length; j++)
			console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "%.2x", data_blocks[i].data[j]);
		console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "\n");
	}

	// Constructing the data header.
	data_header.common.dst_is_a_group = (calltype == DMR_CALL_TYPE_GROUP);
	data_header.common.response_requested = 1;
	data_header.common.dst_llid = dstid;
	data_header.common.src_llid = srcid;
	data_header.common.data_packet_format = DMRPACKET_DATA_HEADER_DPF_SHORT_DATA_DEFINED;
	data_header.common.service_access_point = DMRPACKET_DATA_HEADER_SAP_SHORT_DATA;

	data_header.short_data_defined.appended_blocks = data_blocks_needed;
	data_header.short_data_defined.dd_format = DMRPACKET_DATA_HEADER_DD_FORMAT_UTF16LE;
	data_header.short_data_defined.resync = 1;
	data_header.short_data_defined.full_message = 1;
	data_header.short_data_defined.bit_padding = 0;

	// Constructing the CSBK preamble.
	csbk.last_block = 1;
	csbk.csbko = DMRPACKET_CSBKO_PREAMBLE;
	csbk.data.preamble.data_follows = 1;
	csbk.data.preamble.dst_is_group = (calltype == DMR_CALL_TYPE_GROUP);
	csbk.data.preamble.csbk_blocks_to_follow = number_of_csbk_preambles_to_send+data_blocks_needed+1; // +1 - header
	csbk.dst_id = dstid;
	csbk.src_id = srcid;

	// Sending CSBK preambles.
	for (i = 0; i < number_of_csbk_preambles_to_send; i++) {
		csbk.data.preamble.csbk_blocks_to_follow--;
		ipscpacket_payload = ipscpacket_construct_payload_csbk(&csbk);
		repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_CSBK, calltype, dstid, srcid, ipscpacket_payload)));
	}

	// Sending data header.
	ipscpacket_payload = ipscpacket_construct_payload_sms_header(&data_header);
	repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_DATA_HEADER, calltype, dstid, srcid, ipscpacket_payload)));

	// Sending data blocks.
	for (i = 0; i < data_blocks_needed; i++) {
		ipscpacket_payload = ipscpacket_construct_payload_data_block_rate_34(&data_blocks[i]);
		repeaters_add_to_ipsc_packet_buffer(repeater, ts, ipscpacket_construct_raw_packet(&repeater->ipaddr, ipscpacket_construct_raw_payload(repeater->slot[ts].ipsc_tx_seqnum++, ts, IPSCPACKET_SLOT_TYPE_3_4_RATE_DATA, calltype, dstid, srcid, ipscpacket_payload)));
	}

	free(data_blocks);
}

static void repeaters_process_ipsc_tx_rawpacketbuf(repeater_t *repeater, dmr_timeslot_t ts) {
	struct timeval currtime = {0,};
	struct timeval difftime = {0,};
	ipscrawpacketbuf_t *ipsc_tx_rawpacketbuf_entry_to_send;

	if (repeater == NULL || ts < 0 || ts > 1 || repeater->slot[ts].ipsc_tx_rawpacketbuf == NULL)
		return;

	gettimeofday(&currtime, NULL);
	timersub(&currtime, &repeater->slot[ts].last_ipsc_packet_sent_time, &difftime);
	if (difftime.tv_sec*1000+difftime.tv_usec/1000 >= 50) { // Sending a frame every x ms.
		console_log(LOGLEVEL_REPEATERS "repeaters [%s]: sending ipsc packet from tx buffer\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
		ipsc_tx_rawpacketbuf_entry_to_send = repeater->slot[ts].ipsc_tx_rawpacketbuf;
		if (repeaters_send_raw_ipsc_packet(repeater, &ipsc_tx_rawpacketbuf_entry_to_send->ipscpacket_raw)) {
			// Sending the packet to our IPSC processing loop too.
			//ipsc_processpacket(&ipsc_tx_rawpacketbuf_entry_to_send->ipscpacket_raw, sizeof(ipscpacket_raw_t));

			// Shifting the buffer.
			repeater->slot[ts].ipsc_tx_rawpacketbuf = repeater->slot[ts].ipsc_tx_rawpacketbuf->next;
			free(ipsc_tx_rawpacketbuf_entry_to_send);
			gettimeofday(&repeater->slot[ts].last_ipsc_packet_sent_time, NULL);
		}
		if (repeater->slot[ts].ipsc_tx_rawpacketbuf == NULL)
			console_log(LOGLEVEL_REPEATERS "repeaters [%s]: tx packet buffer got empty\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
	}
	if (repeater->slot[ts].ipsc_tx_rawpacketbuf != NULL)
		daemon_poll_setmaxtimeout(0);
}

void repeaters_process(void) {
	repeater_t *repeater = repeaters;
	repeater_t *repeater_to_remove;
	struct timeval currtime = {0,};
	struct timeval difftime = {0,};

	while (repeater) {
		repeaters_process_ipsc_tx_rawpacketbuf(repeater, 0);
		repeaters_process_ipsc_tx_rawpacketbuf(repeater, 1);

		if (time(NULL)-repeater->last_active_time > config_get_repeaterinactivetimeoutinsec()) {
			console_log(LOGLEVEL_REPEATERS "repeaters [%s]: timed out\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
			repeater_to_remove = repeater;
			repeater = repeater->next;
			repeaters_remove(repeater_to_remove);
			continue;
		}

		if (!repeater->snmpignored && config_get_repeaterinfoupdateinsec() > 0 && time(NULL)-repeater->last_repeaterinfo_request_time > config_get_repeaterinfoupdateinsec()) {
			console_log(LOGLEVEL_REPEATERS LOGLEVEL_DEBUG "repeaters [%s]: sending snmp info update request\n", repeaters_get_display_string_for_ip(&repeater->ipaddr));
			snmp_start_read_repeaterinfo(comm_get_ip_str(&repeater->ipaddr));
			repeater->last_repeaterinfo_request_time = time(NULL);
		}

		if (repeater->slot[0].state == REPEATER_SLOT_STATE_CALL_RUNNING && time(NULL)-repeater->slot[0].last_packet_received_at > config_get_calltimeoutinsec())
			dmr_handle_voicecall_timeout(repeater, 0);

		if (repeater->slot[1].state == REPEATER_SLOT_STATE_CALL_RUNNING && time(NULL)-repeater->slot[1].last_packet_received_at > config_get_calltimeoutinsec())
			dmr_handle_voicecall_timeout(repeater, 1);

		if (repeater->auto_rssi_update_enabled_at > 0 && repeater->auto_rssi_update_enabled_at <= time(NULL)) {
			if (config_get_rssiupdateduringcallinmsec() > 0) {
				gettimeofday(&currtime, NULL);
				timersub(&currtime, &repeater->last_rssi_request_time, &difftime);
				if (difftime.tv_sec*1000+difftime.tv_usec/1000 > config_get_rssiupdateduringcallinmsec()) {
					snmp_start_read_repeaterstatus(comm_get_ip_str(&repeater->ipaddr));
					repeater->last_rssi_request_time = currtime;
				}
			}
		}

		if (repeater->slot[0].state == REPEATER_SLOT_STATE_DATA_RECEIVE_RUNNING && time(NULL)-repeater->slot[0].data_header_received_at > config_get_datatimeoutinsec())
			dmr_handle_data_timeout(repeater, 0);

		if (repeater->slot[1].state == REPEATER_SLOT_STATE_DATA_RECEIVE_RUNNING && time(NULL)-repeater->slot[1].data_header_received_at > config_get_datatimeoutinsec())
			dmr_handle_data_timeout(repeater, 1);

		repeater = repeater->next;
	}
}

void repeaters_deinit(void) {
	console_log("repeaters: deinit\n");

	while (repeaters != NULL)
		repeaters_remove(repeaters);
}
