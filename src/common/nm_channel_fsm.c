/* NM Radio Carrier FSM */

/* (C) 2020 by sysmocom - s.f.m.c. GmbH <info@sysmocom.de>
 * Author: Pau Espin Pedrol <pespin@sysmocom.de>
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

#include <errno.h>
#include <unistd.h>
#include <inttypes.h>

#include <osmocom/core/fsm.h>
#include <osmocom/core/tdef.h>
#include <osmocom/gsm/protocol/gsm_12_21.h>

#include <osmo-bts/logging.h>
#include <osmo-bts/gsm_data.h>
#include <osmo-bts/bts_model.h>
#include <osmo-bts/bts.h>
#include <osmo-bts/rsl.h>
#include <osmo-bts/nm_common_fsm.h>

#define X(s) (1 << (s))

#define nm_chan_fsm_state_chg(fi, NEXT_STATE) \
	osmo_fsm_inst_state_chg(fi, NEXT_STATE, 0, 0)

/* Can the TS be enabled (OPSTARTed)? aka should it stay in "Disabled Dependency" state? */
static bool ts_can_be_enabled(const struct gsm_bts_trx_ts *ts)
{
	return (ts->trx->bb_transc.mo.nm_state.operational == NM_OPSTATE_ENABLED &&
		(!bts_internal_flag_get(ts->trx->bts, BTS_INTERNAL_FLAG_NM_RCHANNEL_DEPENDS_RCARRIER) ||
		 ts->trx->mo.nm_state.operational == NM_OPSTATE_ENABLED));
}

//////////////////////////
// FSM STATE ACTIONS
//////////////////////////

static void st_op_disabled_notinstalled_on_enter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;
	/* Reset state: */
	gsm_ts_release(ts);
	if (ts->vamos.peer)
		gsm_ts_release(ts->vamos.peer);
	TALLOC_FREE(ts->mo.nm_attr);

	ts->mo.setattr_success = false;
	ts->mo.opstart_success = false;
	oml_mo_state_chg(&ts->mo, NM_OPSTATE_DISABLED, NM_AVSTATE_NOT_INSTALLED, NM_STATE_LOCKED);
}

static void st_op_disabled_notinstalled(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;

	switch (event) {
	case NM_EV_OML_UP:
		/* Report current state: */
		oml_tx_state_changed(&ts->mo);
		return;
	case NM_EV_SW_ACT:
		oml_mo_tx_sw_act_rep(&ts->mo);
		if (ts_can_be_enabled(ts))
			nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_DISABLED_OFFLINE);
		else
			nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_DISABLED_DEPENDENCY);
		return;
	default:
		OSMO_ASSERT(0);
	}
}

static void st_op_disabled_dependency_on_enter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;
	ts->mo.opstart_success = false;
	oml_mo_state_chg(&ts->mo, NM_OPSTATE_DISABLED, NM_AVSTATE_DEPENDENCY, -1);
}

static void st_op_disabled_dependency(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;
	struct nm_fsm_ev_setattr_data *setattr_data;
	int rc;

	switch (event) {
	case NM_EV_OML_UP:
		/* Report current state: */
		oml_tx_state_changed(&ts->mo);
		return;
	case NM_EV_RX_SETATTR:
		setattr_data = (struct nm_fsm_ev_setattr_data *)data;
		rc = bts_model_apply_oml(ts->trx->bts, setattr_data->msg,
					 &ts->mo, ts);
		ts->mo.setattr_success = rc == 0;
		oml_fom_ack_nack_copy_msg(setattr_data->msg, rc);
		break;
	case NM_EV_RX_OPSTART:
		LOGPFSML(fi, LOGL_NOTICE, "BSC trying to activate TS while still in avail=dependency. "
			 "Allowing it to stay backward-compatible with older osmo-bts versions, but BSC is wrong.\n");
		if (!ts->mo.setattr_success) {
			oml_mo_opstart_nack(&ts->mo, NM_NACK_CANT_PERFORM);
			return;
		}
		bts_model_opstart(ts->trx->bts, &ts->mo, ts);
		break;
	case NM_EV_OPSTART_ACK:
		ts->mo.opstart_success = true;
		oml_mo_opstart_ack(&ts->mo);
		nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_ENABLED);
		return;
	case NM_EV_OPSTART_NACK:
		ts->mo.opstart_success = false;
		oml_mo_opstart_nack(&ts->mo, (int)(intptr_t)data);
		return;
	case NM_EV_BBTRANSC_ENABLED:
	case NM_EV_RCARRIER_ENABLED:
		if (ts_can_be_enabled(ts))
			nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_DISABLED_OFFLINE);
		return;
	case NM_EV_BBTRANSC_DISABLED:
	case NM_EV_RCARRIER_DISABLED:
		/* do nothing, we are simply waiting for (potentially) both to be enabled */
		return;
	default:
		OSMO_ASSERT(0);
	}
}

static void st_op_disabled_offline_on_enter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;
	ts->mo.opstart_success = false;
	oml_mo_state_chg(&ts->mo, NM_OPSTATE_DISABLED, NM_AVSTATE_OFF_LINE, -1);
}

static void st_op_disabled_offline(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;
	struct nm_fsm_ev_setattr_data *setattr_data;
	int rc;

	switch (event) {
	case NM_EV_OML_UP:
		/* Report current state: */
		oml_tx_state_changed(&ts->mo);
		return;
	case NM_EV_RX_SETATTR:
		setattr_data = (struct nm_fsm_ev_setattr_data *)data;
		rc = bts_model_apply_oml(ts->trx->bts, setattr_data->msg,
					 &ts->mo, ts);
		ts->mo.setattr_success = rc == 0;
		oml_fom_ack_nack_copy_msg(setattr_data->msg, rc);
		break;
	case NM_EV_RX_OPSTART:
		if (!ts->mo.setattr_success) {
			oml_mo_opstart_nack(&ts->mo, NM_NACK_CANT_PERFORM);
			return;
		}
		bts_model_opstart(ts->trx->bts, &ts->mo, ts);
		break;
	case NM_EV_OPSTART_ACK:
		ts->mo.opstart_success = true;
		oml_mo_opstart_ack(&ts->mo);
		nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_ENABLED);
		return;
	case NM_EV_OPSTART_NACK:
		ts->mo.opstart_success = false;
		oml_mo_opstart_nack(&ts->mo, (int)(intptr_t)data);
		return;
	case NM_EV_BBTRANSC_DISABLED:
	case NM_EV_RCARRIER_DISABLED:
		if (!ts_can_be_enabled(ts))
			nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_DISABLED_DEPENDENCY);
		return;
	default:
		OSMO_ASSERT(0);
	}
}

static void st_op_enabled_on_enter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;
	oml_mo_state_chg(&ts->mo, NM_OPSTATE_ENABLED, NM_AVSTATE_OK, -1);
}

static void st_op_enabled(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;

	switch (event) {
	case NM_EV_BBTRANSC_DISABLED:
	case NM_EV_RCARRIER_DISABLED:
		if (!ts_can_be_enabled(ts))
			nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_DISABLED_DEPENDENCY);
		return;
	case NM_EV_DISABLE:
		nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_DISABLED_OFFLINE);
		return;
	default:
		OSMO_ASSERT(0);
	}
}

static void nm_chan_allstate(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_trx_ts *ts = (struct gsm_bts_trx_ts *)fi->priv;

	switch (event) {
	case NM_EV_SHUTDOWN_START:
		/* Announce we start shutting down */
		oml_mo_state_chg(&ts->mo, -1, -1, NM_STATE_SHUTDOWN);
		break;
	case NM_EV_SHUTDOWN_FINISH:
		nm_chan_fsm_state_chg(fi, NM_CHAN_ST_OP_DISABLED_NOTINSTALLED);
		break;
	default:
		OSMO_ASSERT(false);
	}
}

static struct osmo_fsm_state nm_chan_fsm_states[] = {
	[NM_CHAN_ST_OP_DISABLED_NOTINSTALLED] = {
		.in_event_mask =
			X(NM_EV_SW_ACT) |
			X(NM_EV_OML_UP),
		.out_state_mask =
			X(NM_CHAN_ST_OP_DISABLED_NOTINSTALLED) |
			X(NM_CHAN_ST_OP_DISABLED_OFFLINE) |
			X(NM_CHAN_ST_OP_DISABLED_DEPENDENCY),
		.name = "DISABLED_NOTINSTALLED",
		.onenter = st_op_disabled_notinstalled_on_enter,
		.action = st_op_disabled_notinstalled,
	},
	[NM_CHAN_ST_OP_DISABLED_DEPENDENCY] = {
		.in_event_mask =
			X(NM_EV_OML_UP) |
			X(NM_EV_RX_SETATTR) |
			X(NM_EV_RX_OPSTART) | /* backward compatibility, buggy BSC */
			X(NM_EV_OPSTART_ACK) | /* backward compatibility, buggy BSC */
			X(NM_EV_OPSTART_NACK) | /* backward compatibility, buggy BSC */
			X(NM_EV_BBTRANSC_ENABLED) |
			X(NM_EV_RCARRIER_ENABLED) |
			X(NM_EV_BBTRANSC_DISABLED) |
			X(NM_EV_RCARRIER_DISABLED),
		.out_state_mask =
			X(NM_CHAN_ST_OP_DISABLED_NOTINSTALLED) |
			X(NM_CHAN_ST_OP_DISABLED_OFFLINE) |
			X(NM_CHAN_ST_OP_ENABLED), /* backward compatibility, buggy BSC */
		.name = "DISABLED_DEPENDENCY",
		.onenter = st_op_disabled_dependency_on_enter,
		.action = st_op_disabled_dependency,
	},
	[NM_CHAN_ST_OP_DISABLED_OFFLINE] = {
		.in_event_mask =
			X(NM_EV_OML_UP) |
			X(NM_EV_RX_SETATTR) |
			X(NM_EV_RX_OPSTART) |
			X(NM_EV_OPSTART_ACK) |
			X(NM_EV_OPSTART_NACK) |
			X(NM_EV_BBTRANSC_DISABLED) |
			X(NM_EV_RCARRIER_DISABLED),
		.out_state_mask =
			X(NM_CHAN_ST_OP_DISABLED_NOTINSTALLED) |
			X(NM_CHAN_ST_OP_ENABLED) |
			X(NM_CHAN_ST_OP_DISABLED_DEPENDENCY),
		.name = "DISABLED_OFFLINE",
		.onenter = st_op_disabled_offline_on_enter,
		.action = st_op_disabled_offline,
	},
	[NM_CHAN_ST_OP_ENABLED] = {
		.in_event_mask =
			X(NM_EV_BBTRANSC_DISABLED) |
			X(NM_EV_RCARRIER_DISABLED) |
			X(NM_EV_DISABLE),
		.out_state_mask =
			X(NM_CHAN_ST_OP_DISABLED_NOTINSTALLED) |
			X(NM_CHAN_ST_OP_DISABLED_OFFLINE) |
			X(NM_CHAN_ST_OP_DISABLED_DEPENDENCY),
		.name = "ENABLED",
		.onenter = st_op_enabled_on_enter,
		.action = st_op_enabled,
	},
};

struct osmo_fsm nm_chan_fsm = {
	.name = "NM_CHAN_OP",
	.states = nm_chan_fsm_states,
	.num_states = ARRAY_SIZE(nm_chan_fsm_states),
	.event_names = nm_fsm_event_names,
	.allstate_action = nm_chan_allstate,
	.allstate_event_mask = X(NM_EV_SHUTDOWN_START) |
			       X(NM_EV_SHUTDOWN_FINISH),
	.log_subsys = DOML,
};

static __attribute__((constructor)) void nm_chan_fsm_init(void)
{
	OSMO_ASSERT(osmo_fsm_register(&nm_chan_fsm) == 0);
}
