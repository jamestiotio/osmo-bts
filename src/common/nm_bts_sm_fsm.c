/* NM BTS Site Manager FSM */

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
#include <osmo-bts/bts_sm.h>
#include <osmo-bts/rsl.h>
#include <osmo-bts/nm_common_fsm.h>
#include <osmo-bts/phy_link.h>

#define X(s) (1 << (s))

#define nm_bts_sm_fsm_state_chg(fi, NEXT_STATE) \
	osmo_fsm_inst_state_chg(fi, NEXT_STATE, 0, 0)


static void ev_dispatch_children(struct gsm_bts_sm *site_mgr, uint32_t event)
{
	struct gsm_bts *bts;
	osmo_fsm_inst_dispatch(site_mgr->gprs.nse.mo.fi, event, NULL);
	llist_for_each_entry(bts, &site_mgr->bts_list, list) {
		osmo_fsm_inst_dispatch(bts->mo.fi, event, NULL);
	}
}

//////////////////////////
// FSM STATE ACTIONS
//////////////////////////

static void st_op_disabled_notinstalled_on_enter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct gsm_bts_sm *site_mgr = (struct gsm_bts_sm *)fi->priv;
	site_mgr->mo.setattr_success = false;
	site_mgr->mo.opstart_success = false;
	oml_mo_state_chg(&site_mgr->mo, NM_OPSTATE_DISABLED, NM_AVSTATE_NOT_INSTALLED, NM_STATE_LOCKED);
}

static void st_op_disabled_notinstalled(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_sm *site_mgr = (struct gsm_bts_sm *)fi->priv;

	switch (event) {
	case NM_EV_OML_UP:
		/* automatic SW_ACT upon OML link establishment: */
		oml_mo_tx_sw_act_rep(&site_mgr->mo);
		nm_bts_sm_fsm_state_chg(fi, NM_BTS_SM_ST_OP_DISABLED_OFFLINE);
		ev_dispatch_children(site_mgr, event);
		return;
	default:
		OSMO_ASSERT(0);
	}
}

static void st_op_disabled_offline_on_enter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct gsm_bts_sm *site_mgr = (struct gsm_bts_sm *)fi->priv;
	site_mgr->mo.setattr_success = false;
	site_mgr->mo.opstart_success = false;
	oml_mo_state_chg(&site_mgr->mo, NM_OPSTATE_DISABLED, NM_AVSTATE_OFF_LINE, -1);
}

static void st_op_disabled_offline(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_sm *site_mgr = (struct gsm_bts_sm *)fi->priv;
	struct nm_fsm_ev_setattr_data *setattr_data;
	int rc;

	switch (event) {
	case NM_EV_RX_SETATTR:
		setattr_data = (struct nm_fsm_ev_setattr_data *)data;
		/* No bts_model_apply_oml() needed yet for site_mgr obj yet: */
		rc = 0;
		site_mgr->mo.setattr_success = rc == 0;
		oml_fom_ack_nack_copy_msg(setattr_data->msg, rc);
		break;
	case NM_EV_RX_OPSTART:
#if 0
		/* Disabled because osmo-bsc doesn't send SetAttr on SITE_MGR object */
		if (!site_mgr->mo.setattr_success) {
			oml_mo_opstart_nack(&site_mgr->mo, NM_NACK_CANT_PERFORM);
			return;
		}
#endif
		bts_model_opstart(NULL, &site_mgr->mo, site_mgr);
		break;
	case NM_EV_OPSTART_ACK:
		site_mgr->mo.opstart_success = true;
		oml_mo_opstart_ack(&site_mgr->mo);
		nm_bts_sm_fsm_state_chg(fi, NM_BTS_SM_ST_OP_ENABLED);
		break; /* check statechg below */
	case NM_EV_OPSTART_NACK:
		site_mgr->mo.opstart_success = false;
		oml_mo_opstart_nack(&site_mgr->mo, (int)(intptr_t)data);
		return;
	default:
		OSMO_ASSERT(0);
	}
}

static void st_op_enabled_on_enter(struct osmo_fsm_inst *fi, uint32_t prev_state)
{
	struct gsm_bts_sm *site_mgr = (struct gsm_bts_sm *)fi->priv;
	oml_mo_state_chg(&site_mgr->mo, NM_OPSTATE_ENABLED, NM_AVSTATE_OK, -1);
}

static void st_op_enabled(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
}

static void nm_bts_sm_allstate(struct osmo_fsm_inst *fi, uint32_t event, void *data)
{
	struct gsm_bts_sm *site_mgr = (struct gsm_bts_sm *)fi->priv;

	switch (event) {
	case NM_EV_SHUTDOWN_START:
		/* Announce we start shutting down */
		oml_mo_state_chg(&site_mgr->mo, -1, -1, NM_STATE_SHUTDOWN);

		/* Propagate event to children: */
		ev_dispatch_children(site_mgr, event);
		break;
	case NM_EV_SHUTDOWN_FINISH:
		/* Propagate event to children: */
		ev_dispatch_children(site_mgr, event);
		nm_bts_sm_fsm_state_chg(fi, NM_BTS_SM_ST_OP_DISABLED_NOTINSTALLED);
		break;
	default:
		OSMO_ASSERT(false);
	}
}

static struct osmo_fsm_state nm_bts_sm_fsm_states[] = {
	[NM_BTS_SM_ST_OP_DISABLED_NOTINSTALLED] = {
		.in_event_mask =
			X(NM_EV_OML_UP),
		.out_state_mask =
			X(NM_BTS_SM_ST_OP_DISABLED_NOTINSTALLED) |
			X(NM_BTS_SM_ST_OP_DISABLED_OFFLINE),
		.name = "DISABLED_NOTINSTALLED",
		.onenter = st_op_disabled_notinstalled_on_enter,
		.action = st_op_disabled_notinstalled,
	},
	[NM_BTS_SM_ST_OP_DISABLED_OFFLINE] = {
		.in_event_mask =
			X(NM_EV_RX_SETATTR) |
			X(NM_EV_RX_OPSTART) |
			X(NM_EV_OPSTART_ACK) |
			X(NM_EV_OPSTART_NACK),
		.out_state_mask =
			X(NM_BTS_SM_ST_OP_DISABLED_NOTINSTALLED) |
			X(NM_BTS_SM_ST_OP_ENABLED),
		.name = "DISABLED_OFFLINE",
		.onenter = st_op_disabled_offline_on_enter,
		.action = st_op_disabled_offline,
	},
	[NM_BTS_SM_ST_OP_ENABLED] = {
		.in_event_mask = 0,
		.out_state_mask =
			X(NM_BTS_SM_ST_OP_DISABLED_NOTINSTALLED),
		.name = "ENABLED",
		.onenter = st_op_enabled_on_enter,
		.action = st_op_enabled,
	},
};

struct osmo_fsm nm_bts_sm_fsm = {
	.name = "NM_BTS_SM_OP",
	.states = nm_bts_sm_fsm_states,
	.num_states = ARRAY_SIZE(nm_bts_sm_fsm_states),
	.event_names = nm_fsm_event_names,
	.allstate_action = nm_bts_sm_allstate,
	.allstate_event_mask = X(NM_EV_SHUTDOWN_START) |
			       X(NM_EV_SHUTDOWN_FINISH),
	.log_subsys = DOML,
};

static __attribute__((constructor)) void nm_bts_sm_fsm_init(void)
{
	OSMO_ASSERT(osmo_fsm_register(&nm_bts_sm_fsm) == 0);
}
