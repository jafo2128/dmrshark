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

// This SMS TX buffer takes care of sending SMSes and retrying when send
// fails. It's a FIFO, and only one (the first) element is tried to be
// sent at a time.

#include DEFAULTCONFIG

#include "smstxbuf.h"
#include "dmr-data.h"
#include "smsrtbuf.h"

#include <libs/daemon/console.h>
#include <libs/daemon/daemon-poll.h>
#include <libs/comm/repeaters.h>
#include <libs/config/config.h>

#include <stdlib.h>
#include <string.h>

static smstxbuf_t *smstxbuf_first_entry = NULL;
static smstxbuf_t *smstxbuf_last_entry = NULL;

void smstxbuf_print_entry(smstxbuf_t *entry) {
	char added_at_str[20];

	strftime(added_at_str, sizeof(added_at_str), "%F %T", localtime(&entry->added_at));
	if (entry->repeater == NULL)
		console_log(LOGLEVEL_DMR "  repeater: all ");
	else {
		console_log(LOGLEVEL_DMR "  repeater: %s ts: %u ",
			repeaters_get_display_string_for_ip(&entry->repeater->ipaddr),
			entry->ts+1);
	}
	console_log("dst id: %u src id: %u type: %s added at: %s send tries: %u type: %s msg: %s\n",
		entry->dst_id, entry->src_id,
		dmr_get_readable_call_type(entry->call_type), added_at_str, entry->send_tries, dmr_get_readable_sms_type(entry->sms_type), entry->msg);
}

void smstxbuf_print(void) {
	smstxbuf_t *entry = smstxbuf_first_entry;

	if (entry == NULL) {
		console_log("smstxbuf: empty\n");
		return;
	}
	console_log("smstxbuf:\n");
	while (entry) {
		smstxbuf_print_entry(entry);
		entry = entry->next;
	}
}

// In case of repeater is 0, the SMS will be sent broadcast.
void smstxbuf_add(repeater_t *repeater, dmr_timeslot_t ts, dmr_call_type_t calltype, dmr_id_t dstid, dmr_id_t srcid, dmr_sms_type_t sms_type, char *msg) {
	smstxbuf_t *new_smstxbuf_entry;
	loglevel_t loglevel;

	if (msg == NULL)
		return;

	new_smstxbuf_entry = (smstxbuf_t *)calloc(1, sizeof(smstxbuf_t));
	if (new_smstxbuf_entry == NULL) {
		console_log("  error: can't allocate memory for new sms buffer entry\n");
		return;
	}

	strncpy(new_smstxbuf_entry->msg, msg, DMRPACKET_MAX_FRAGMENTSIZE);
	new_smstxbuf_entry->added_at = time(NULL);
	new_smstxbuf_entry->sms_type = sms_type;
	new_smstxbuf_entry->call_type = calltype;
	new_smstxbuf_entry->dst_id = dstid;
	new_smstxbuf_entry->src_id = srcid;
	new_smstxbuf_entry->repeater = repeater;
	new_smstxbuf_entry->ts = ts;

	console_log(LOGLEVEL_DMR "smstxbuf: adding new sms:\n");
	loglevel = console_get_loglevel();
	if (loglevel.flags.dmr)
		smstxbuf_print_entry(new_smstxbuf_entry);

	if (smstxbuf_last_entry == NULL) {
		smstxbuf_last_entry = smstxbuf_first_entry = new_smstxbuf_entry;
	} else {
		// Putting the new entry to the end of the linked list.
		smstxbuf_last_entry->next = new_smstxbuf_entry;
		smstxbuf_last_entry = new_smstxbuf_entry;
	}
	daemon_poll_setmaxtimeout(0);
}

static void smstxbuf_remove_first_entry(void) {
	smstxbuf_t *nextentry;
	loglevel_t loglevel;

	if (smstxbuf_first_entry == NULL)
		return;

	loglevel = console_get_loglevel();
	if (loglevel.flags.dmr && loglevel.flags.debug) {
		console_log(LOGLEVEL_DMR LOGLEVEL_DEBUG "smstxbuf: removing first entry:\n");
		smstxbuf_print_entry(smstxbuf_first_entry);
	}

	nextentry = smstxbuf_first_entry->next;
	free(smstxbuf_first_entry);
	smstxbuf_first_entry = nextentry;
	if (smstxbuf_first_entry == NULL)
		smstxbuf_last_entry = NULL;
}

void smstxbuf_first_entry_sent_successfully(void) {
	smsrtbuf_t *smsrtbuf_entry;

	if (smstxbuf_first_entry == NULL)
		return;

	smsrtbuf_entry = smsrtbuf_find_entry(smstxbuf_first_entry->dst_id, smstxbuf_first_entry->msg);

	console_log(LOGLEVEL_DMR "smstxbuf: first entry sent successfully\n");
	if (smsrtbuf_entry != NULL)
		smsrtbuf_entry_sent_successfully(smsrtbuf_entry);
	smstxbuf_remove_first_entry();
}

static void smstxbuf_first_entry_send_unsuccessful(void) {
	smsrtbuf_t *smsrtbuf_entry;

	if (smstxbuf_first_entry == NULL)
		return;

	smsrtbuf_entry = smsrtbuf_find_entry(smstxbuf_first_entry->dst_id, smstxbuf_first_entry->msg);

	console_log(LOGLEVEL_DMR "smstxbuf: first entry send unsuccessful\n");
	if (smsrtbuf_entry != NULL)
		smsrtbuf_entry_send_unsuccessful(smsrtbuf_entry);
	smstxbuf_remove_first_entry();
}

smstxbuf_t *smstxbuf_get_first_entry(void) {
	return smstxbuf_first_entry;
}

void smstxbuf_process(void) {
	static time_t last_sms_send_try_at = 0;
	loglevel_t loglevel;

	if (smstxbuf_first_entry == NULL)
		return;

	if (time(NULL)-last_sms_send_try_at < config_get_smssendretryintervalinsec()) {
		daemon_poll_setmaxtimeout(config_get_smssendretryintervalinsec()-(time(NULL)-last_sms_send_try_at));
		return;
	}

	if (smstxbuf_first_entry->send_tries >= config_get_smssendmaxretrycount()) {
		console_log(LOGLEVEL_DMR "smstxbuf: all tries of sending the first entry has failed\n");
		smstxbuf_print_entry(smstxbuf_first_entry);
		smstxbuf_first_entry_send_unsuccessful();
		if (smstxbuf_first_entry == NULL)
			return;
	}

	smstxbuf_first_entry->selective_ack_tries = 0;
	loglevel = console_get_loglevel();
	if (loglevel.flags.dmr) {
		console_log(LOGLEVEL_DMR "smstxbuf: sending entry:\n");
		smstxbuf_print_entry(smstxbuf_first_entry);
	}

	switch (smstxbuf_first_entry->sms_type) {
		case DMR_SMS_TYPE_MOTOROLA_TMS:
			dmr_data_send_motorola_tms_sms((smstxbuf_first_entry->repeater == NULL), smstxbuf_first_entry->repeater, smstxbuf_first_entry->ts, smstxbuf_first_entry->call_type, smstxbuf_first_entry->dst_id, smstxbuf_first_entry->src_id, smstxbuf_first_entry->msg);
			break;
		case DMR_SMS_TYPE_NORMAL:
			dmr_data_send_sms((smstxbuf_first_entry->repeater == NULL), smstxbuf_first_entry->repeater, smstxbuf_first_entry->ts, smstxbuf_first_entry->call_type, smstxbuf_first_entry->dst_id, smstxbuf_first_entry->src_id, smstxbuf_first_entry->msg);
			break;
		default:
			break;
	}

	if (smstxbuf_first_entry->call_type == DMR_CALL_TYPE_GROUP) // Group messages are unconfirmed, so we send them only once.
		smstxbuf_remove_first_entry();
	else
		smstxbuf_first_entry->send_tries++;
	last_sms_send_try_at = time(NULL);
	daemon_poll_setmaxtimeout(0);
}

void smstxbuf_deinit(void) {
	smstxbuf_t *next_entry;

	while (smstxbuf_first_entry != NULL) {
		next_entry = smstxbuf_first_entry->next;
		free(smstxbuf_first_entry);
		smstxbuf_first_entry = next_entry;
	}
	smstxbuf_last_entry = NULL;
}
