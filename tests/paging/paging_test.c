/* testing the paging code */

/* (C) 2011 by Holger Hans Peter Freyther
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <osmocom/core/talloc.h>
#include <osmocom/core/application.h>

#include <osmo-bts/bts.h>
#include <osmo-bts/bts_sm.h>
#include <osmo-bts/logging.h>
#include <osmo-bts/paging.h>
#include <osmo-bts/gsm_data.h>
#include <osmo-bts/l1sap.h>
#include <osmo-bts/notification.h>

#include <unistd.h>

static struct gsm_bts *bts;

static const uint8_t static_ilv[] = {
	0x08, 0x59, 0x51, 0x30, 0x99, 0x00, 0x00, 0x00, 0x19
};

#define ASSERT_TRUE(rc) \
	if (!(rc)) { \
		printf("Assert failed in %s:%d.\n",  \
		       __FILE__, __LINE__);          \
		abort();			     \
	}

static bool is_padding(const uint8_t *in, size_t len)
{
	int i;
	for (i = 0; i < len; i++) {
		if (in[i] != 0x2b)
			return false;
	}
	return true;
}

static void test_paging_smoke(void)
{
	int rc;
	uint8_t out_buf[GSM_MACBLOCK_LEN];
	struct gsm_time g_time;
	int is_empty = -1;
	printf("Testing that paging messages expire.\n");

	/* add paging entry */
	rc = paging_add_identity(bts->paging_state, 0, static_ilv, 0);
	ASSERT_TRUE(rc == 0);
	ASSERT_TRUE(paging_queue_length(bts->paging_state) == 1);

	/* generate messages */
	g_time.fn = 0;
	g_time.t1 = 0;
	g_time.t2 = 0;
	g_time.t3 = 6;
	rc = paging_gen_msg(bts->paging_state, out_buf, &g_time, &is_empty);
	ASSERT_TRUE(rc == 23);
	ASSERT_TRUE(is_padding(out_buf+13, 23-13));
	ASSERT_TRUE(is_empty == 0);

	ASSERT_TRUE(paging_group_queue_empty(bts->paging_state, 0));
	ASSERT_TRUE(paging_queue_length(bts->paging_state) == 0);

	/* now test the empty queue */
	g_time.fn = 0;
	g_time.t1 = 0;
	g_time.t2 = 0;
	g_time.t3 = 6;
	rc = paging_gen_msg(bts->paging_state, out_buf, &g_time, &is_empty);
	ASSERT_TRUE(rc == 23);
	ASSERT_TRUE(is_padding(out_buf+6, 23-6));
	ASSERT_TRUE(is_empty == 1);

	/*
	 * TODO: test all the cases of different amount tmsi/imsi and check
	 * if we fill the slots in a optimal way.
	 */
}

static void test_paging_sleep(void)
{
	int rc;
	uint8_t out_buf[GSM_MACBLOCK_LEN];
	struct gsm_time g_time;
	int is_empty = -1;
	printf("Testing that paging messages expire with sleep.\n");

	/* add paging entry */
	rc = paging_add_identity(bts->paging_state, 0, static_ilv, 0);
	ASSERT_TRUE(rc == 0);
	ASSERT_TRUE(paging_queue_length(bts->paging_state) == 1);

	/* sleep */
	sleep(1);

	/* generate messages */
	g_time.fn = 0;
	g_time.t1 = 0;
	g_time.t2 = 0;
	g_time.t3 = 6;
	rc = paging_gen_msg(bts->paging_state, out_buf, &g_time, &is_empty);
	ASSERT_TRUE(rc == 23);
	ASSERT_TRUE(is_padding(out_buf+13, 23-13));
	ASSERT_TRUE(is_empty == 0);

	ASSERT_TRUE(paging_group_queue_empty(bts->paging_state, 0));
	ASSERT_TRUE(paging_queue_length(bts->paging_state) == 0);
}

/* Set up a dummy trx with a valid setting for bs_ag_blks_res in SI3 */
static struct gsm_bts_trx *test_is_ccch_for_agch_setup(uint8_t bs_ag_blks_res)
{
	static struct gsm_bts_trx trx;
	static struct gsm_bts bts;
	struct gsm48_system_information_type_3 si3 = { 0 };
	si3.control_channel_desc.bs_ag_blks_res = bs_ag_blks_res;
	trx.bts = &bts;
	bts.si_valid |= 0x8;
	bts.asci.pos_nch = -1;
	memcpy(&bts.si_buf[SYSINFO_TYPE_3][0], &si3, sizeof(si3));
	return &trx;
}

/* Walk through all possible settings for bs_ag_blks_res for two
 * multiframe 51. The patterns shown in 3GPP TS 05.02 Clause 7
 * Table 5 of 9 must occur. */
static void test_is_ccch_for_agch(void)
{
	enum ccch_msgt ccch;
	int fn;
	uint8_t bs_ag_blks_res;
	struct gsm_bts_trx *trx;

	printf("Fn:   AGCH: (bs_ag_blks_res=[0:7]\n");
	for (fn = 0; fn < 102; fn++) {
		uint8_t fn51 = fn % 51;
		/* Note: the formula that computes the CCCH block number for a
		 * given frame number is optimized to work on block boarders,
		 * for frame numbers that do not fall at the beginning of the
		 * related block this formula would produce wrong results, so
		 * we only check with frame numbers that mark the beginning
		 * of a new block. See also L1SAP_FN2CCCHBLOCK() in l1sap.h */

		if (fn51 % 10 != 2 && fn51 % 10 != 6)
			continue;

		printf("%03u: ", fn);

		if (fn51 == 2) {
			printf(" . . . . . . . . (BCCH)\n");
			continue;
		}

		/* Try allo possible settings for bs_ag_blks_res */
		for (bs_ag_blks_res = 0; bs_ag_blks_res <= 7; bs_ag_blks_res++) {
			trx = test_is_ccch_for_agch_setup(bs_ag_blks_res);
			ccch = get_ccch_msgt(trx, fn);
			printf(" %u", (ccch == CCCH_MSGT_AGCH));
		}
		printf("\n");
	}
}

static void test_paging_rest_octets1(void)
{
	uint8_t out_buf[17];
	struct p1_rest_octets p1ro = {};
	struct asci_notification notif = {};

	struct bitvec bv = {
		.data_len = sizeof(out_buf),
		.data = out_buf,
	};

	/* no rest */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	append_p1_rest_octets(&bv, &p1ro, NULL);
	ASSERT_TRUE(out_buf[0] == 0x2b);

	/* add NLN */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p1ro.nln_pch.present = true;
	p1ro.nln_pch.nln = 3;
	p1ro.nln_pch.nln_status = 1;
	append_p1_rest_octets(&bv, &p1ro, NULL);
	ASSERT_TRUE(out_buf[0] == 0xfb); /* H 1 11 1 */
	p1ro.nln_pch.present = 0;

	/* add group callref */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	notif.group_call_ref[0] = 0x12;
	notif.group_call_ref[1] = 0x34;
	notif.group_call_ref[2] = 0x56;
	notif.group_call_ref[3] = 0x78;
	notif.group_call_ref[4] = 0x90;
	notif.chan_desc.present = true;
	notif.chan_desc.len = 3;
	notif.chan_desc.value[0] = 0x20;
	notif.chan_desc.value[1] = 0x40;
	notif.chan_desc.value[2] = 0x80;
	append_p1_rest_octets(&bv, &p1ro, &notif);
	ASSERT_TRUE(out_buf[0] == 0x31); /* L L L H 0x123456789 */
	ASSERT_TRUE(out_buf[1] == 0x23);
	ASSERT_TRUE(out_buf[2] == 0x45);
	ASSERT_TRUE(out_buf[3] == 0x67);
	ASSERT_TRUE(out_buf[4] == 0x89);
	ASSERT_TRUE(out_buf[5] == 0x90); /* H 0x204080 0 */
	ASSERT_TRUE(out_buf[6] == 0x20);
	ASSERT_TRUE(out_buf[7] == 0x40);
	ASSERT_TRUE(out_buf[8] == 0x2b);

	/* add Packet Page Indication 1 */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p1ro.packet_page_ind[0] = true;
	append_p1_rest_octets(&bv, &p1ro, NULL);
	ASSERT_TRUE(out_buf[0] == 0x23); /* L L L L H L L L */
	p1ro.packet_page_ind[0] = false;

	/* add Packet Page Indication 2 */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p1ro.packet_page_ind[1] = true;
	append_p1_rest_octets(&bv, &p1ro, NULL);
	ASSERT_TRUE(out_buf[0] == 0x2f); /* L L L L L H L L */
	p1ro.packet_page_ind[1] = false;

	/* add ETWS */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p1ro.r8_present = true;
	p1ro.r8.prio_ul_access = true;
	p1ro.r8.etws_present = true;
	p1ro.r8.etws.is_first = true;
	p1ro.r8.etws.page_nr = 0x5;
	uint8_t page[] = { 0x22, 0x44, 0x66 };
	p1ro.r8.etws.page_bytes = sizeof(page);
	p1ro.r8.etws.page = page;
	append_p1_rest_octets(&bv, &p1ro, NULL);
	ASSERT_TRUE(out_buf[0] == 0x2b); /* L L L L L L L L */
	ASSERT_TRUE(out_buf[1] == 0xe5); /* H 1 1 0 0x5 */
	ASSERT_TRUE(out_buf[2] == 0x18); /* 0 len=24=0x18 */
	ASSERT_TRUE(out_buf[3] == 0x22); /* 0x224488 */
	ASSERT_TRUE(out_buf[4] == 0x44);
	ASSERT_TRUE(out_buf[5] == 0x66);
	p1ro.r8_present = false;
}

static void test_paging_rest_octets2(void)
{
	uint8_t out_buf[11];
	struct p2_rest_octets p2ro = {};

	struct bitvec bv = {
		.data_len = sizeof(out_buf),
		.data = out_buf,
	};

	/* nothing added */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	append_p2_rest_octets(&bv, &p2ro);
	ASSERT_TRUE(out_buf[0] == 0x2b); /* L L */

	/* add cneed */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p2ro.cneed.present = true;
	p2ro.cneed.cn3 = 3;
	append_p2_rest_octets(&bv, &p2ro);
	ASSERT_TRUE(out_buf[0] == 0xeb); /* H 1 1 L */
	p2ro.cneed.present = false;

	/* add NLN */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p2ro.nln_pch.present = true;
	p2ro.nln_pch.nln = 3;
	p2ro.nln_pch.nln_status = 1;
	append_p2_rest_octets(&bv, &p2ro);
	ASSERT_TRUE(out_buf[0] == 0x7b); /* L H 1 11 1 */
	p2ro.nln_pch.present = 0;
}

static void test_paging_rest_octets3(void)
{
	uint8_t out_buf[3];
	struct p3_rest_octets p3ro = {};

	struct bitvec bv = {
		.data_len = sizeof(out_buf),
		.data = out_buf,
	};

	/* nothing added */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	append_p3_rest_octets(&bv, &p3ro);
	ASSERT_TRUE(out_buf[0] == 0x2b); /* L L */

	/* add cneed */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p3ro.cneed.present = true;
	p3ro.cneed.cn3 = 3;
	p3ro.cneed.cn4 = 3;
	append_p3_rest_octets(&bv, &p3ro);
	ASSERT_TRUE(out_buf[0] == 0xfb); /* H 1 1 1 1 L */
	p3ro.cneed.present = false;

	/* add NLN */
	memset(out_buf, GSM_MACBLOCK_PADDING, sizeof(out_buf));
	bv.cur_bit = 0;
	p3ro.nln_pch.present = true;
	p3ro.nln_pch.nln = 3;
	p3ro.nln_pch.nln_status = 1;
	append_p3_rest_octets(&bv, &p3ro);
	ASSERT_TRUE(out_buf[0] == 0x7b); /* L H 1 11 1 */
	p3ro.nln_pch.present = 0;
}

int main(int argc, char **argv)
{
	tall_bts_ctx = talloc_named_const(NULL, 1, "OsmoBTS context");
	msgb_talloc_ctx_init(tall_bts_ctx, 0);

	osmo_init_logging2(tall_bts_ctx, &bts_log_info);

	g_bts_sm = gsm_bts_sm_alloc(tall_bts_ctx);
	if (!g_bts_sm) {
		fprintf(stderr, "Failed to create BTS Site Manager structure\n");
		exit(1);
	}
	bts = gsm_bts_alloc(g_bts_sm, 0);
	if (bts_init(bts) < 0) {
		fprintf(stderr, "unable to open bts\n");
		exit(1);
	}

	test_paging_smoke();
	test_paging_sleep();
	test_is_ccch_for_agch();
	test_paging_rest_octets1();
	test_paging_rest_octets2();
	test_paging_rest_octets3();
	printf("Success\n");

	return 0;
}

