
#include <stdint.h>
#include <errno.h>

#include <osmocom/core/utils.h>
#include <osmocom/core/endian.h>

#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/protocol/gsm_44_004.h>

#include <osmo-bts/gsm_data.h>
#include <osmo-bts/logging.h>
#include <osmo-bts/measurement.h>
#include <osmo-bts/scheduler.h>
#include <osmo-bts/rsl.h>
#include <osmo-bts/power_control.h>
#include <osmo-bts/ta_control.h>

/* Active TDMA frame subset for TCH/H in DTX mode (see 3GPP TS 45.008 Section 8.3).
 * This mapping is used to determine if a L2 block starting at the given TDMA FN
 * belongs to the SUB set and thus shall always be transmitted in DTX mode. */
static const uint8_t ts45008_dtx_tchh_fn_map[104] = {
	/* TCH/H(0): 0, 2, 4, 6, 52, 54, 56, 58 */
	[0]  = 1, /* block { 0,  2,  4,  6} */
	[52] = 1, /* block {52, 54, 56, 58} */
	/* TCH/H(1): 14, 16, 18, 20, 66, 68, 70, 72 */
	[14] = 1, /* block {14, 16, 18, 20} */
	[66] = 1, /* block {66, 68, 70, 72} */
};

/* In cases where we less measurements than we expect we must assume that we
 * just did not receive the block because it was lost due to bad channel
 * conditions. We set up a dummy measurement result here that reflects the
 * worst possible result. In our* calculation we will use this dummy to replace
 * the missing measurements */
#define MEASUREMENT_DUMMY_BER 10000 /* 100% BER */
#define MEASUREMENT_DUMMY_IRSSI 109 /* noise floor in -dBm */
static const struct bts_ul_meas measurement_dummy = {
	.ber10k = MEASUREMENT_DUMMY_BER,
	.ta_offs_256bits = 0,
	.ci_cb = 0,
	.is_sub = 0,
	.inv_rssi = MEASUREMENT_DUMMY_IRSSI
};

/* Decide if a given frame number is part of the "-SUB" measurements (true) or not (false)
 * (this function is only used internally, it is public to call it from unit-tests) */
bool ts45008_83_is_sub(struct gsm_lchan *lchan, uint32_t fn)
{
	uint32_t fn104 = fn % 104;

	/* See TS 45.008 Sections 8.3 and 8.4 for a detailed descriptions of the rules
	 * implemented here. We only implement the logic for Voice, not CSD */

	/* AMR is special, SID frames may be scheduled dynamically at any time */
	if (lchan->tch_mode == GSM48_CMODE_SPEECH_AMR)
		return false;

	switch (lchan->type) {
	case GSM_LCHAN_TCH_F:
		switch (lchan->tch_mode) {
		case GSM48_CMODE_SPEECH_V1:
		case GSM48_CMODE_SPEECH_EFR:
			/* Active TDMA frame subset for TCH/F: 52, 53, 54, 55, 56, 57, 58, 59.
			 * There is only one *complete* block in this subset starting at FN=52.
			 * Incomplete blocks {... 52, 53, 54, 55} and {56, 57, 58, 59 ...}
			 * contain only 50% of the useful bits (partial SID) and thus ~50% BER. */
			if (fn104 == 52)
				return true;
			break;
		case GSM48_CMODE_SIGN:
			/* No DTX allowed; SUB=FULL, therefore measurements at all frame numbers are
			 * SUB */
			return true;
		case GSM48_CMODE_DATA_12k0: /* TCH/F9.6 */
		case GSM48_CMODE_DATA_6k0: /* TCH/F4.8 */
			/* FIXME: In case of data traffic channels TCH/F9.6 and TCH/F4.8 the
			 * RXQUAL_SUB report shall include measurements on the TDMA frames given
			 * in the table of subclause 8.3 only if L2 fill frames have been received
			 * as FACCH/F frames at the corresponding frame positions. */
		default:
			if (lchan->rsl_cmode == RSL_CMOD_SPD_DATA)
				return false;
			LOGPLCFN(lchan, fn, DMEAS, LOGL_ERROR, "Unsupported lchan->tch_mode %u\n", lchan->tch_mode);
			break;
		}
		break;
	case GSM_LCHAN_TCH_H:
		switch (lchan->tch_mode) {
		case GSM48_CMODE_SPEECH_V1:
			if (ts45008_dtx_tchh_fn_map[fn104])
				return true;
			break;
		case GSM48_CMODE_SIGN:
			/* No DTX allowed; SUB=FULL, therefore measurements at all frame numbers are
			 * SUB */
			return true;
		case GSM48_CMODE_DATA_6k0: /* TCH/H4.8 */
		case GSM48_CMODE_DATA_3k6: /* TCH/H2.4 */
			/* FIXME: In case of data traffic channels TCH/H4.8 and TCH/H2.4 the
			 * RXQUAL_SUB report shall include measurements on the TDMA frames given
			 * in the table of subclause 8.3 only if L2 fill frames have been received
			 * as FACCH/H frames at the corresponding frame positions. */
		default:
			if (lchan->rsl_cmode == RSL_CMOD_SPD_DATA)
				return false;
			LOGPLCFN(lchan, fn, DMEAS, LOGL_ERROR, "Unsupported lchan->tch_mode %u\n", lchan->tch_mode);
			break;
		}
		break;
	case GSM_LCHAN_SDCCH:
		/* No DTX allowed; SUB=FULL, therefore measurements at all frame numbers are SUB */
		return true;
	default:
		break;
	}
	return false;
}

/* Measurement reporting period and mapping of SACCH message block for TCHF
 * and TCHH chan As per in 3GPP TS 45.008, section 8.4.1.
 *
 *             Timeslot number (TN)        TDMA frame number (FN) modulo 104
 *             Half rate,    Half rate,     Reporting    SACCH
 * Full Rate   subch.0       subch.1        period       Message block
 * 0           0 and 1                      0 to 103     12,  38,  64,  90
 * 1                         0 and 1        13 to 12     25,  51,  77,  103
 * 2           2 and 3                      26 to 25     38,  64,  90,  12
 * 3                         2 and 3        39 to 38     51,  77,  103, 25
 * 4           4 and 5                      52 to 51     64,  90,  12,  38
 * 5                         4 and 5        65 to 64     77,  103, 25,  51
 * 6           6 and 7                      78 to 77     90,  12,  38,  64
 * 7                         6 and 7        91 to 90     103, 25,  51,  77
 *
 * Note: The array index of the following three lookup tables refes to a
 *       timeslot number. */

static const uint8_t tchf_meas_rep_fn104_by_ts[] = {
	[0] =	90,
	[1] =	103,
	[2] =	12,
	[3] =	25,
	[4] =	38,
	[5] =	51,
	[6] =	64,
	[7] =	77,
};
static const uint8_t tchh0_meas_rep_fn104_by_ts[] = {
	[0] =	90,
	[1] =	90,
	[2] =	12,
	[3] =	12,
	[4] =	38,
	[5] =	38,
	[6] =	64,
	[7] =	64,
};
static const uint8_t tchh1_meas_rep_fn104_by_ts[] = {
	[0] =	103,
	[1] =	103,
	[2] =	25,
	[3] =	25,
	[4] =	51,
	[5] =	51,
	[6] =	77,
	[7] =	77,
};

/* Measurement reporting period for SDCCH8 and SDCCH4 chan
 * As per in 3GPP TS 45.008, section 8.4.2.
 *
 * Logical Chan		TDMA frame number
 *			(FN) modulo 102
 *
 * SDCCH/8		12 to 11
 * SDCCH/4		37 to 36
 *
 *
 * Note: The array index of the following three lookup tables refes to a
 *       subslot number. */

/* FN of the first burst whose block completes before reaching fn%102=11 */
static const uint8_t sdcch8_meas_rep_fn102_by_ss[] = {
	[0] = 66,	/* 15(SDCCH), 47(SACCH), 66(SDCCH) */
	[1] = 70,	/* 19(SDCCH), 51(SACCH), 70(SDCCH) */
	[2] = 74,	/* 23(SDCCH), 55(SACCH), 74(SDCCH) */
	[3] = 78,	/* 27(SDCCH), 59(SACCH), 78(SDCCH) */
	[4] = 98,	/* 31(SDCCH), 98(SACCH), 82(SDCCH) */
	[5] = 0,	/* 35(SDCCH),  0(SACCH), 86(SDCCH) */
	[6] = 4,	/* 39(SDCCH),  4(SACCH), 90(SDCCH) */
	[7] = 8,	/* 43(SDCCH),  8(SACCH), 94(SDCCH) */
};

/* FN of the first burst whose block completes before reaching fn%102=37 */
static const uint8_t sdcch4_meas_rep_fn102_by_ss[] = {
	[0] = 88,	/* 37(SDCCH), 57(SACCH), 88(SDCCH) */
	[1] = 92,	/* 41(SDCCH), 61(SACCH), 92(SDCCH) */
	[2] = 6,	/*  6(SACCH), 47(SDCCH), 98(SDCCH) */
	[3] = 10	/* 10(SACCH),  0(SDCCH), 51(SDCCH) */
};

/* Note: The reporting of the measurement results is done via the SACCH channel.
 * The measurement interval is not aligned with the interval in which the
 * SACCH is transmitted. When we receive the measurement indication with the
 * SACCH block, the corresponding measurement interval will already have ended
 * and we will get the results late, but on spot with the beginning of the
 * next measurement interval.
 *
 * For example: We get a measurement indication on FN%104=38 in TS=2. Then we
 * will have to look at 3GPP TS 45.008, section 8.4.1 (or 3GPP TS 05.02 Clause 7
 * Table 1 of 9) what value we need to feed into the lookup tables in order to
 * detect the measurement period ending. In this example the "real" ending
 * was on FN%104=12. This is the value we have to look for in
 * tchf_meas_rep_fn104_by_ts to know that a measurement period has just ended. */

/* See also 3GPP TS 05.02 Clause 7 Table 1 of 9:
 * Mapping of logical channels onto physical channels (see subclauses 6.3, 6.4, 6.5) */
static uint8_t translate_tch_meas_rep_fn104(uint8_t fn_mod)
{
	switch (fn_mod) {
	case 25:
		return 103;
	case 38:
		return 12;
	case 51:
		return 25;
	case 64:
		return 38;
	case 77:
		return 51;
	case 90:
		return 64;
	case 103:
		return 77;
	case 12:
		return 90;
	}

	/* Invalid / not of interest */
	return 0;
}

/* determine if a measurement period ends at the given frame number
 * (this function is only used internally, it is public to call it from
 * unit-tests) */
int is_meas_complete(struct gsm_lchan *lchan, uint32_t fn)
{
	unsigned int fn_mod = -1;
	const uint8_t *tbl;
	int rc = 0;
	enum gsm_phys_chan_config pchan = ts_pchan(lchan->ts);

	switch (pchan) {
	case GSM_PCHAN_TCH_F:
		fn_mod = translate_tch_meas_rep_fn104(fn % 104);
		if (tchf_meas_rep_fn104_by_ts[lchan->ts->nr] == fn_mod)
			rc = 1;
		break;
	case GSM_PCHAN_TCH_H:
		fn_mod = translate_tch_meas_rep_fn104(fn % 104);
		if (lchan->nr == 0)
			tbl = tchh0_meas_rep_fn104_by_ts;
		else
			tbl = tchh1_meas_rep_fn104_by_ts;
		if (tbl[lchan->ts->nr] == fn_mod)
			rc = 1;
		break;
	case GSM_PCHAN_SDCCH8_SACCH8C:
	case GSM_PCHAN_SDCCH8_SACCH8C_CBCH:
		fn_mod = fn % 102;
		if (sdcch8_meas_rep_fn102_by_ss[lchan->nr] == fn_mod)
			rc = 1;
		break;
	case GSM_PCHAN_CCCH_SDCCH4:
	case GSM_PCHAN_CCCH_SDCCH4_CBCH:
		fn_mod = fn % 102;
		if (sdcch4_meas_rep_fn102_by_ss[lchan->nr] == fn_mod)
			rc = 1;
		break;
	default:
		rc = 0;
		break;
	}

	if (rc == 1) {
		LOGPLCFN(lchan, fn, DMEAS, LOGL_DEBUG, "meas period end fn_mod:%d, status:%d, pchan:%s\n", fn_mod,
			 rc, gsm_pchan_name(pchan));
	}

	return rc;
}

/* determine the measurement interval modulus by a given lchan */
static uint8_t modulus_by_lchan(struct gsm_lchan *lchan)
{
	enum gsm_phys_chan_config pchan = ts_pchan(lchan->ts);

	switch (pchan) {
	case GSM_PCHAN_TCH_F:
	case GSM_PCHAN_TCH_H:
		return 104;
	case GSM_PCHAN_SDCCH8_SACCH8C:
	case GSM_PCHAN_SDCCH8_SACCH8C_CBCH:
	case GSM_PCHAN_CCCH_SDCCH4:
	case GSM_PCHAN_CCCH_SDCCH4_CBCH:
		return 102;
	default:
		/* Invalid */
		return 1;
	}
}

/* receive a L1 uplink measurement from L1 (this function is only used
 * internally, it is public to call it from unit-tests)  */
int lchan_new_ul_meas(struct gsm_lchan *lchan,
		      const struct bts_ul_meas *ulm,
		      uint32_t fn)
{
	uint32_t fn_mod = fn % modulus_by_lchan(lchan);
	struct bts_ul_meas *dest;

	if (lchan->state != LCHAN_S_ACTIVE) {
		LOGPLCFN(lchan, fn, DMEAS, LOGL_NOTICE,
			 "measurement during state: %s, num_ul_meas=%d, fn_mod=%u\n",
			 gsm_lchans_name(lchan->state), lchan->meas.num_ul_meas, fn_mod);
	}

	if (lchan->meas.num_ul_meas >= ARRAY_SIZE(lchan->meas.uplink)) {
		LOGPLCFN(lchan, fn, DMEAS, LOGL_NOTICE,
			 "no space for uplink measurement, num_ul_meas=%d, fn_mod=%u\n", lchan->meas.num_ul_meas,
			 fn_mod);
		return -ENOSPC;
	}

	dest = &lchan->meas.uplink[lchan->meas.num_ul_meas++];
	memcpy(dest, ulm, sizeof(*ulm));

	/* We expect the lower layers to mark AMR SID_UPDATE frames already as such.
	 * In this function, we only deal with the common logic as per the TS 45.008 tables */
	if (!ulm->is_sub)
		dest->is_sub = ts45008_83_is_sub(lchan, fn);

	LOGPLCFN(lchan, fn, DMEAS, LOGL_DEBUG,
		 "adding a %s measurement (ber10k=%u, ta_offs=%d, ci_cB=%d, rssi=-%u), num_ul_meas=%d, fn_mod=%u\n",
		 dest->is_sub ? "SUB" : "FULL", ulm->ber10k, ulm->ta_offs_256bits, ulm->ci_cb, ulm->inv_rssi,
		 lchan->meas.num_ul_meas, fn_mod);

	lchan->meas.last_fn = fn;

	return 0;
}

/* input: BER in steps of .01%, i.e. percent/100 */
static uint8_t ber10k_to_rxqual(uint32_t ber10k)
{
	/* Eight levels of Rx quality are defined and are mapped to the
	 * equivalent BER before channel decoding, as per in 3GPP TS 45.008,
	 * secton 8.2.4.
	 *
	 * RxQual:				BER Range:
	 * RXQUAL_0	     BER <  0,2 %       Assumed value = 0,14 %
	 * RXQUAL_1  0,2 % < BER <  0,4 %	Assumed value = 0,28 %
	 * RXQUAL_2  0,4 % < BER <  0,8 %	Assumed value = 0,57 %
	 * RXQUAL_3  0,8 % < BER <  1,6 %	Assumed value = 1,13 %
	 * RXQUAL_4  1,6 % < BER <  3,2 %	Assumed value = 2,26 %
	 * RXQUAL_5  3,2 % < BER <  6,4 %	Assumed value = 4,53 %
	 * RXQUAL_6  6,4 % < BER < 12,8 %	Assumed value = 9,05 %
	 * RXQUAL_7 12,8 % < BER		Assumed value = 18,10 % */

	if (ber10k < 20)
		return 0;
	if (ber10k < 40)
		return 1;
	if (ber10k < 80)
		return 2;
	if (ber10k < 160)
		return 3;
	if (ber10k < 320)
		return 4;
	if (ber10k < 640)
		return 5;
	if (ber10k < 1280)
		return 6;
	return 7;
}

/* Get the number of measurements that we expect for a specific lchan.
 * (This is a static number that is defined by the specific slot layout of
 * the channel) */
static unsigned int lchan_meas_num_expected(const struct gsm_lchan *lchan)
{
	enum gsm_phys_chan_config pchan = ts_pchan(lchan->ts);

	switch (pchan) {
	case GSM_PCHAN_TCH_F:
		/* 24 blocks for TCH + 1 for SACCH */
		return 25;
	case GSM_PCHAN_TCH_H:
		if (lchan->tch_mode == GSM48_CMODE_SIGN) {
			/* 12 blocks for TCH + 1 for SACCH */
			return 13;
		} else {
			/* 24 blocks for TCH + 1 for SACCH */
			return 25;
		}
	case GSM_PCHAN_SDCCH8_SACCH8C:
	case GSM_PCHAN_SDCCH8_SACCH8C_CBCH:
		/* 2 for SDCCH + 1 for SACCH */
		return 3;
	case GSM_PCHAN_CCCH_SDCCH4:
	case GSM_PCHAN_CCCH_SDCCH4_CBCH:
		/* 2 for SDCCH + 1 for SACCH */
		return 3;
	default:
		return lchan->meas.num_ul_meas;
	}
}

/* In DTX a subset of blocks must always be transmitted
 * See also: GSM 05.08, chapter 8.3 Aspects of discontinuous transmission (DTX) */
static unsigned int lchan_meas_sub_num_expected(const struct gsm_lchan *lchan)
{
	enum gsm_phys_chan_config pchan = ts_pchan(lchan->ts);

	/* AMR is using a more elaborated model with a dymanic amount of
	 * DTX blocks so this function is not applicable to determine the
	 * amount of expected SUB Measurements when AMR is used */
	OSMO_ASSERT(lchan->tch_mode != GSM48_CMODE_SPEECH_AMR);

	switch (pchan) {
	case GSM_PCHAN_TCH_F:
		if (lchan->tch_mode == GSM48_CMODE_SIGN) {
			/* 1 block SACCH, 24 blocks TCH (see note 1) */
			return 25;
		} else {
			/* 1 block SACCH, 1 block TCH */
			return 2;
		}
	case GSM_PCHAN_TCH_H:
		if (lchan->tch_mode == GSM48_CMODE_SIGN) {
			/* 1 block SACCH, 12 blocks TCH (see ynote 1) */
			return 13;
		} else {
			/* 1 block SACCH, 2 blocks TCH */
			return 3;
		}
	case GSM_PCHAN_SDCCH8_SACCH8C:
	case GSM_PCHAN_SDCCH8_SACCH8C_CBCH:
		/* no DTX here, all blocks must be present! */
		return 3;
	case GSM_PCHAN_CCCH_SDCCH4:
	case GSM_PCHAN_CCCH_SDCCH4_CBCH:
		/* no DTX here, all blocks must be present! */
		return 3;
	default:
		return 0;
	}

	/* Note 1: In signalling mode all blocks count as SUB blocks. */
}

/* if we clip the TOA value to 12 bits, i.e. toa256=3200,
 *  -> the maximum deviation can be 2*3200 = 6400
 *  -> the maximum squared deviation can be 6400^2 = 40960000
 *  -> the maximum sum of squared deviations can be 104*40960000 = 4259840000
 *     and hence fit into uint32_t
 *  -> once the value is divided by 104, it's again below 40960000
 *     leaving 6 MSBs of freedom, i.e. we could extend by 64, resulting in 2621440000
 *  -> as a result, the standard deviation could be communicated with up to six bits
 *     of fractional fixed-point number.
 */

/* compute Osmocom extended measurements for the given lchan */
static void lchan_meas_compute_extended(struct gsm_lchan *lchan)
{
	unsigned int num_ul_meas;
	unsigned int num_ul_meas_excess = 0;
        unsigned int num_ul_meas_expect;

	/* we assume that lchan_meas_check_compute() has already computed the mean value
	 * and we can compute the min/max/variance/stddev from this */
	int i;

	/* each measurement is an int32_t, so the squared difference value must fit in 32bits */
	/* the sum of the squared values (each up to 32bit) can very easily exceed 32 bits */
	uint64_t sq_diff_sum = 0;

	/* In case we do not have any measurement values collected there is no
	 * computation possible. We just skip the whole computation here, the
	 * lchan->meas.flags will not get the LC_UL_M_F_OSMO_EXT_VALID flag set
	 * so no extended measurement results will be reported back via RSL.
	 * this is ok, since we have nothing to report anyway and apart of that
	 * we also just lost the signal (otherwise we would have at least some
	 * measurements). */
	if (!lchan->meas.num_ul_meas)
		return;

	/* initialize min/max values with their counterpart */
	lchan->meas.ext.toa256_min = INT16_MAX;
	lchan->meas.ext.toa256_max = INT16_MIN;

	/* Determine the number of measurement values we need to take into the
	 * computation. In this case we only compute over the measurements we
	 * have indeed received. Since this computation is about timing
	 * information it does not make sense to approach missing measurement
	 * samples the TOA with 0. This would bend the average towards 0. What
	 * counts is the average TOA of the properly received blocks so that
	 * the TA logic can make a proper decision. */
        num_ul_meas_expect = lchan_meas_num_expected(lchan);
	if (lchan->meas.num_ul_meas > num_ul_meas_expect) {
		num_ul_meas = num_ul_meas_expect;
		num_ul_meas_excess = lchan->meas.num_ul_meas - num_ul_meas_expect;
	}
	else
		num_ul_meas = lchan->meas.num_ul_meas;

	/* all computations are done on the relative arrival time of the burst, relative to the
	 * beginning of its slot. This is of course excluding the TA value that the MS has already
	 * compensated/pre-empted its transmission */

	/* step 1: compute the sum of the squared difference of each value to mean */
	for (i = 0; i < num_ul_meas; i++) {
		const struct bts_ul_meas *m;

		OSMO_ASSERT(i < lchan->meas.num_ul_meas);
		m = &lchan->meas.uplink[i+num_ul_meas_excess];

		int32_t diff = (int32_t)m->ta_offs_256bits - (int32_t)lchan->meas.ms_toa256;
		/* diff can now be any value of +65535 to -65535, so we can safely square it,
		 * but only in unsigned math.  As squaring looses the sign, we can simply drop
		 * it before squaring, too. */
		uint32_t diff_abs = labs(diff);
		uint32_t diff_squared = diff_abs * diff_abs;
		sq_diff_sum += diff_squared;

		/* also use this loop iteration to compute min/max values */
		if (m->ta_offs_256bits > lchan->meas.ext.toa256_max)
			lchan->meas.ext.toa256_max = m->ta_offs_256bits;
		if (m->ta_offs_256bits < lchan->meas.ext.toa256_min)
			lchan->meas.ext.toa256_min = m->ta_offs_256bits;
	}
	/* step 2: compute the variance (mean of sum of squared differences) */
	sq_diff_sum = sq_diff_sum / num_ul_meas;
	/* as the individual summed values can each not exceed 2^32, and we're
	 * dividing by the number of summands, the resulting value can also not exceed 2^32 */
	OSMO_ASSERT(sq_diff_sum <= UINT32_MAX);
	/* step 3: compute the standard deviation from the variance */
	lchan->meas.ext.toa256_std_dev = osmo_isqrt32(sq_diff_sum);
	lchan->meas.flags |= LC_UL_M_F_OSMO_EXT_VALID;
}

int lchan_meas_check_compute(struct gsm_lchan *lchan, uint32_t fn)
{
	struct gsm_meas_rep_unidir *mru;
	uint32_t ber_full_sum = 0;
	uint32_t irssi_full_sum = 0;
	int32_t ci_full_sum = 0;
	uint32_t ber_sub_sum = 0;
	uint32_t irssi_sub_sum = 0;
	int32_t ci_sub_sum = 0;
	int32_t ta256b_sum = 0;
	unsigned int num_meas_sub = 0;
	unsigned int num_meas_sub_actual = 0;
	unsigned int num_meas_sub_subst = 0;
	unsigned int num_meas_sub_expect;
	unsigned int num_ul_meas;
	unsigned int num_ul_meas_actual = 0;
	unsigned int num_ul_meas_subst = 0;
	unsigned int num_ul_meas_expect;
	unsigned int num_ul_meas_excess = 0;
	int i;

	/* if measurement period is not complete, abort */
	if (!is_meas_complete(lchan, fn))
		return 0;

	LOGPLCHAN(lchan, DMEAS, LOGL_DEBUG, "Calculating measurement results "
		  "for physical channel: %s\n", gsm_pchan_name(ts_pchan(lchan->ts)));

	/* Note: Some phys will send no measurement indication at all
	 * when a block is lost. Also in DTX mode blocks are left out
	 * intentionally to save energy. It is not necessarly an error
	 * when we get less measurements as we expect. */
	num_ul_meas_expect = lchan_meas_num_expected(lchan);

	if (lchan->tch_mode != GSM48_CMODE_SPEECH_AMR)
		num_meas_sub_expect = lchan_meas_sub_num_expected(lchan);
	else {
		/* When AMR is used, we expect at least one SUB frame, since
		 * the SACCH will always be SUB frame. There may occur more
		 * SUB frames but since DTX periods in AMR are dynamic, we
		 * can not know how many exactly. */
		num_meas_sub_expect = 1;
	}

	if (lchan->meas.num_ul_meas > num_ul_meas_expect)
		num_ul_meas_excess = lchan->meas.num_ul_meas - num_ul_meas_expect;
	num_ul_meas = num_ul_meas_expect;

	LOGPLCHAN(lchan, DMEAS, LOGL_DEBUG, "Received %u UL measurements, expected %u\n",
		  lchan->meas.num_ul_meas, num_ul_meas_expect);
	if (num_ul_meas_excess)
		LOGPLCHAN(lchan, DMEAS, LOGL_DEBUG, "Received %u excess UL measurements\n",
			  num_ul_meas_excess);

	/* Measurement computation step 1: add up */
	for (i = 0; i < num_ul_meas; i++) {
		const struct bts_ul_meas *m;
		bool is_sub = false;

		/* Note: We will always compute over a full measurement,
		 * interval even when not enough measurement samples are in
		 * the buffer. As soon as we run out of measurement values
		 * we continue the calculation using dummy values. This works
		 * well for the BER, since there we can safely assume 100%
		 * since a missing measurement means that the data (block)
		 * is lost as well (some phys do not give us measurement
		 * reports for lost blocks or blocks that are spaced out for
		 * DTX). However, for RSSI and TA this does not work since
		 * there we would distort the calculation if we would replace
		 * them with a made up number. This means for those values we
		 * only compute over the data we have actually received. */

		if (i < lchan->meas.num_ul_meas) {
			m = &lchan->meas.uplink[i + num_ul_meas_excess];
			if (m->is_sub) {
				irssi_sub_sum += m->inv_rssi;
				ci_sub_sum += m->ci_cb;
				num_meas_sub_actual++;
				is_sub = true;
			}
			irssi_full_sum += m->inv_rssi;
			ta256b_sum += m->ta_offs_256bits;
			ci_full_sum += m->ci_cb;

			num_ul_meas_actual++;
		} else {
			m = &measurement_dummy;

			/* For AMR the amount of SUB frames is defined by the
			 * the occurrence of DTX periods, which are dynamically
			 * negotiated in AMR, so we can not know if and how many
			 * SUB frames are missing. */
			if (lchan->tch_mode != GSM48_CMODE_SPEECH_AMR) {
				if (num_meas_sub <= i) {
					num_meas_sub_subst++;
					is_sub = true;
				}
			}

			num_ul_meas_subst++;
		}

		ber_full_sum += m->ber10k;
		if (is_sub) {
			num_meas_sub++;
			ber_sub_sum += m->ber10k;
		}
	}

	if (lchan->tch_mode != GSM48_CMODE_SPEECH_AMR) {
		LOGPLCHAN(lchan, DMEAS, LOGL_DEBUG,
			  "Received UL measurements contain %u SUB measurements, expected %u\n",
			  num_meas_sub_actual, num_meas_sub_expect);
	} else {
		LOGPLCHAN(lchan, DMEAS, LOGL_DEBUG,
			  "Received UL measurements contain %u SUB measurements, expected at least %u\n",
			  num_meas_sub_actual, num_meas_sub_expect);
	}

	LOGPLCHAN(lchan, DMEAS, LOGL_DEBUG, "Replaced %u measurements with dummy values, "
		  "from which %u were SUB measurements\n", num_ul_meas_subst, num_meas_sub_subst);

	/* Normally the logic above should make sure that there is
	 * always the exact amount of SUB measurements taken into
	 * account. If not then the logic that decides tags the received
	 * measurements as is_sub works incorrectly. Since the logic
	 * above only adds missing measurements during the calculation
	 * it can not remove excess SUB measurements or add missing SUB
	 * measurements when there is no more room in the interval. */
	if (lchan->tch_mode != GSM48_CMODE_SPEECH_AMR) {
		if (num_meas_sub != num_meas_sub_expect) {
			LOGPLCHAN(lchan, DMEAS, LOGL_ERROR,
				  "Incorrect number of SUB measurements detected! "
				  "(%u vs exp %u)\n", num_meas_sub, num_meas_sub_expect);
		}
	} else {
		if (num_meas_sub < num_meas_sub_expect) {
			LOGPLCHAN(lchan, DMEAS, LOGL_ERROR,
				  "Incorrect number of SUB measurements detected! "
				  "(%u vs exp >=%u)\n", num_meas_sub, num_meas_sub_expect);
		}
	}

	/* Measurement computation step 2: divide */
	ber_full_sum = ber_full_sum / num_ul_meas;

	if (!irssi_full_sum)
		irssi_full_sum = MEASUREMENT_DUMMY_IRSSI;
	else
		irssi_full_sum = irssi_full_sum / num_ul_meas_actual;

	if (!num_ul_meas_actual) {
		ta256b_sum = lchan->meas.ms_toa256;
		ci_full_sum = lchan->meas.ul_ci_cb_full;
	} else {
		ta256b_sum = ta256b_sum / (signed)num_ul_meas_actual;
		ci_full_sum = ci_full_sum / (signed)num_ul_meas_actual;
	}

	if (!num_meas_sub)
		ber_sub_sum = MEASUREMENT_DUMMY_BER;
	else
		ber_sub_sum = ber_sub_sum / num_meas_sub;

	if (!num_meas_sub_actual) {
		irssi_sub_sum = MEASUREMENT_DUMMY_IRSSI;
		ci_sub_sum = lchan->meas.ul_ci_cb_sub;
	} else {
		irssi_sub_sum = irssi_sub_sum / num_meas_sub_actual;
		ci_sub_sum = ci_sub_sum / (signed)num_meas_sub_actual;
	}

	LOGPLCHAN(lchan, DMEAS, LOGL_INFO,
		  "Computed TA256(% 4d), BER-FULL(%2u.%02u%%), RSSI-FULL(-%3udBm), C/I-FULL(% 4d cB), "
		  "BER-SUB(%2u.%02u%%), RSSI-SUB(-%3udBm), C/I-SUB(% 4d cB)\n",
		  ta256b_sum, ber_full_sum / 100, ber_full_sum % 100, irssi_full_sum, ci_full_sum,
		  ber_sub_sum / 100, ber_sub_sum % 100, irssi_sub_sum, ci_sub_sum);

	/* store results */
	mru = &lchan->meas.ul_res;
	mru->full.rx_lev = dbm2rxlev((int)irssi_full_sum * -1);
	mru->sub.rx_lev = dbm2rxlev((int)irssi_sub_sum * -1);
	mru->full.rx_qual = ber10k_to_rxqual(ber_full_sum);
	mru->sub.rx_qual = ber10k_to_rxqual(ber_sub_sum);
	lchan->meas.ms_toa256 = ta256b_sum;
	lchan->meas.ul_ci_cb_full = ci_full_sum;
	lchan->meas.ul_ci_cb_sub = ci_sub_sum;

	LOGPLCHAN(lchan, DMEAS, LOGL_INFO,
		  "UL MEAS RXLEV_FULL(%u), RXLEV_SUB(%u), RXQUAL_FULL(%u), RXQUAL_SUB(%u), "
		  "num_meas_sub(%u), num_ul_meas(%u)\n",
		  mru->full.rx_lev, mru->sub.rx_lev,
		  mru->full.rx_qual, mru->sub.rx_qual,
		  num_meas_sub, num_ul_meas_expect);

	lchan->meas.flags |= LC_UL_M_F_RES_VALID;

	lchan_meas_compute_extended(lchan);

	lchan->meas.num_ul_meas = 0;

	/* return 1 to indicate that the computation has been done and the next
	 * interval begins. */
	return 1;
}

/* Process a single uplink measurement sample. This function is called from
 * l1sap.c every time a measurement indication is received. It collects the
 * measurement samples and automatically detects the end of the measurement
 * interval. */
int lchan_meas_process_measurement(struct gsm_lchan *lchan,
				   const struct bts_ul_meas *ulm,
				   uint32_t fn)
{
	lchan_new_ul_meas(lchan, ulm, fn);
	return lchan_meas_check_compute(lchan, fn);
}

/* Reset all measurement related struct members to their initial values. This
 * function will be called every time an lchan is activated to ensure the
 * measurement process starts with a defined state. */
void lchan_meas_reset(struct gsm_lchan *lchan)
{
	memset(&lchan->meas, 0, sizeof(lchan->meas));
	lchan->meas.last_fn = LCHAN_FN_DUMMY;
}

static inline uint8_t ms_to2rsl(const struct gsm_lchan *lchan, uint8_t ta)
{
	return (lchan->ms_t_offs >= 0) ? lchan->ms_t_offs : (lchan->p_offs - ta);
}

static inline bool ms_to_valid(const struct gsm_lchan *lchan)
{
	return (lchan->ms_t_offs >= 0) || (lchan->p_offs >= 0);
}

/* Decide if repeated FACCH should be applied or not. If RXQUAL level, that the
 * MS reports is high enough, FACCH repetition is not needed. */
static void repeated_dl_facch_active_decision(struct gsm_lchan *lchan,
					      const struct gsm48_meas_res *meas_res)
{
	uint8_t upper;
	uint8_t lower;
	uint8_t rxqual;
	bool prev_repeated_dl_facch_active = lchan->rep_acch.dl_facch_active;

	/* This is an optimization so that we exit as quickly as possible if
	 * there are no FACCH repetition capabilities present. However If the
	 * repeated FACCH capabilities vanish for whatever reason, we must be
	 * sure that FACCH repetition is disabled. */
	if (!lchan->rep_acch_cap.dl_facch_cmd
	    && !lchan->rep_acch_cap.dl_facch_all) {
		lchan->rep_acch.dl_facch_active = false;
		goto out;
	}

	/* Threshold disabled (always on) */
	if (lchan->rep_acch_cap.rxqual == 0) {
		lchan->rep_acch.dl_facch_active = true;
		goto out;
	}

	/* When the MS sets the SRR bit in the UL-SACCH L1 header
	 * (repeated SACCH requested) then it makes sense to enable
	 * FACCH repetition too. */
	if (lchan->meas.l1_info.srr_sro) {
		lchan->rep_acch.dl_facch_active = true;
		goto out;
	}

	/* Parse MS measurement results */
	if (meas_res == NULL)
		goto out;
	if (!gsm48_meas_res_is_valid(meas_res))
		goto out;

	/* If the RXQUAL level at the MS drops under a certain threshold
	 * we enable FACCH repetition. */
	upper = lchan->rep_acch_cap.rxqual;
	if (upper > 2)
		lower = lchan->rep_acch_cap.rxqual - 2;
	else
		lower = 0;

	/* When downlink DTX is applied, use RXQUAL-SUB, otherwise use
	 * RXQUAL-FULL. */
	if (meas_res->dtx_used)
		rxqual = meas_res->rxqual_sub;
	else
		rxqual = meas_res->rxqual_full;

	if (rxqual >= upper)
		lchan->rep_acch.dl_facch_active = true;
	else if (rxqual <= lower)
		lchan->rep_acch.dl_facch_active = false;

out:
	if (lchan->rep_acch.dl_facch_active == prev_repeated_dl_facch_active)
		return;
	if (lchan->rep_acch.dl_facch_active)
		LOGPLCHAN(lchan, DL1P, LOGL_DEBUG, "DL-FACCH repetition: inactive => active\n");
	else
		LOGPLCHAN(lchan, DL1P, LOGL_DEBUG, "DL-FACCH repetition: active => inactive\n");
}

static void acch_overpower_active_decision(struct gsm_lchan *lchan,
					   const struct gsm48_meas_res *meas_res)
{
	const bool old = lchan->top_acch_active;
	uint8_t upper, lower, rxqual;

	/* ACCH overpower is not allowed => nothing to do */
	if (lchan->top_acch_cap.overpower_db == 0)
		return;
	/* RxQual threshold is disabled => overpower is always on */
	if (lchan->top_acch_cap.rxqual == 0)
		return;

	/* If DTx is active on Downlink, use the '-SUB' */
	if (meas_res->dtx_used)
		rxqual = meas_res->rxqual_sub;
	else /* ... otherwise use the '-FULL' */
		rxqual = meas_res->rxqual_full;

	upper = lchan->top_acch_cap.rxqual;
	if (upper > 2)
		lower = upper - 2;
	else
		lower = 0;

	if (rxqual >= upper)
		lchan->top_acch_active = true;
	else if (rxqual <= lower)
		lchan->top_acch_active = false;

	if (lchan->top_acch_active != old) {
		LOGPLCHAN(lchan, DL1P, LOGL_DEBUG, "Temporary ACCH overpower: %s\n",
			  lchan->top_acch_active ? "inactive => active"
						  : "active => inactive");
	}
}

static bool data_is_rr_meas_rep(const uint8_t *data)
{
	const struct gsm48_hdr *gh = (void *)(data + 5);
	const uint8_t *lapdm_hdr = (void *)(data + 2);

	/* LAPDm address field: SAPI=0, C/R=0, EA=1 */
	if (lapdm_hdr[0] != 0x01)
		return false;
	/* LAPDm control field: U, func=UI */
	if (lapdm_hdr[1] != 0x03)
		return false;
	/* Protocol discriminator: RR */
	if (gh->proto_discr != GSM48_PDISC_RR)
		return false;

	switch (gh->msg_type) {
	case GSM48_MT_RR_EXT_MEAS_REP:
	case GSM48_MT_RR_MEAS_REP:
		return true;
	default:
		return false;
	}
}

/* Called every time a SACCH block is received from lower layers */
void lchan_meas_handle_sacch(struct gsm_lchan *lchan, struct msgb *msg)
{
	const struct gsm48_meas_res *mr = NULL;
	const struct gsm48_hdr *gh = NULL;
	int timing_offset, rc;
	bool dtxu_used = true; /* safe default assumption */
	uint8_t ms_pwr;
	uint8_t ms_ta;
	int8_t ul_rssi;
	int16_t ul_ci_cb;
	uint8_t *l3;
	unsigned int l3_len;

	if (msgb_l2len(msg) == GSM_MACBLOCK_LEN) {
		/* Some brilliant engineer decided that the ordering of
		 * fields on the Um interface is different from the
		 * order of fields in RSL. See 3GPP TS 44.004 (section 7.2)
		 * vs. 3GPP TS 48.058 (section 9.3.10). */
		const struct gsm_sacch_l1_hdr *l1h = msgb_l2(msg);
		lchan->meas.l1_info.ms_pwr = l1h->ms_pwr;
		lchan->meas.l1_info.fpc_epc = l1h->fpc_epc;
		lchan->meas.l1_info.srr_sro = l1h->srr_sro;
		lchan->meas.l1_info.ta = l1h->ta;
		lchan->meas.flags |= LC_UL_M_F_L1_VALID;

		/* Check if this is a Measurement Report */
		if (data_is_rr_meas_rep(msgb_l2(msg))) {
			/* Skip both L1 SACCH and LAPDm headers */
			msg->l3h = (void *)(msg->l2h + 2 + 3);
			gh = msgb_l3(msg);
		}

		ms_pwr = lchan->meas.l1_info.ms_pwr;
		ms_ta = lchan->meas.l1_info.ta;
	} else {
		lchan->meas.flags &= ~LC_UL_M_F_L1_VALID;
		ms_pwr = lchan->ms_power_ctrl.current;
		ms_ta = lchan->ta_ctrl.current;
	}

	timing_offset = ms_to_valid(lchan) ? ms_to2rsl(lchan, ms_ta) : -1;
	l3 = msgb_l3(msg);
	l3_len = l3 ? msgb_l3len(msg) : 0;
	rc = rsl_tx_meas_res(lchan, l3, l3_len, timing_offset);
	if (rc == 0) /* Count successful transmissions */
		lchan->meas.res_nr++;

	/* Run control loops now that we have all the information: */
	/* 3GPP TS 45.008 sec 4.2: UL L1 SACCH Header contains TA and
	 * MS_PWR used "for the last burst of the previous SACCH
	 * period". Since MS must use the values provided in DL SACCH
	 * starting at next meas period, the value of the "last burst"
	 * is actually the value used in the entire meas period. Since
	 * it contains info about the previous meas period, we want to
	 * feed the Control Loop with the measurements for the same
	 * period (the previous one), which is stored in lchan->meas(.ul_res):
	 */
	if (gh && gh->msg_type == GSM48_MT_RR_MEAS_REP) {
		mr = (const struct gsm48_meas_res *)gh->data;
		if (gsm48_meas_res_is_valid(mr))
			dtxu_used = mr->dtx_used;
	}

	if (dtxu_used) {
		ul_rssi = rxlev2dbm(lchan->meas.ul_res.sub.rx_lev);
		ul_ci_cb = lchan->meas.ul_ci_cb_sub;
	} else {
		ul_rssi = rxlev2dbm(lchan->meas.ul_res.full.rx_lev);
		ul_ci_cb = lchan->meas.ul_ci_cb_full;
	}
	lchan_ms_ta_ctrl(lchan, ms_ta, lchan->meas.ms_toa256);
	lchan_ms_pwr_ctrl(lchan, ms_pwr, ul_rssi, ul_ci_cb);
	if (mr && gsm48_meas_res_is_valid(mr)) {
		lchan_bs_pwr_ctrl(lchan, mr);
		acch_overpower_active_decision(lchan, mr);
	}

	repeated_dl_facch_active_decision(lchan, mr);

	/* Reset state for next iteration */
	lchan->tch.dtx.dl_active = false;
	lchan->meas.flags &= ~LC_UL_M_F_OSMO_EXT_VALID;
	lchan->ms_t_offs = -1;
	lchan->p_offs = -1;
}
