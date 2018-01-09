/******************************************************************************
 *	Copyright (C) 2017 Marvell International Ltd.
 *
 *  If you received this File from Marvell, you may opt to use, redistribute
 *  and/or modify this File under the following licensing terms.
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *	* Redistributions of source code must retain the above copyright
 *	  notice, this list of conditions and the following disclaimer.
 *
 *	* Redistributions in binary form must reproduce the above copyright
 *	  notice, this list of conditions and the following disclaimer in the
 *	  documentation and/or other materials provided with the distribution.
 *
 *	* Neither the name of Marvell nor the names of its contributors may be
 *	  used to endorse or promote products derived from this software
 *	  without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#define log_fmt(fmt) "pf: " fmt

#include "std_internal.h"
#include "hw_emul/gie.h"
#include "drivers/mv_mqa.h"
#include "drivers/mv_mqa_queue.h"
#include "drivers/mv_giu_gpio.h"
#include "pf_mng_cmd_desc.h"
#include "mng/db.h"
#include "mng/mv_nmp.h"
#include "mng/pci_ep_def.h"
#include "mng/dispatch.h"
#include "pf_regfile.h"
#include "pf.h"
#include "pf_pci_if_desc.h"
#include "drivers/mv_pp2.h"
#include "drivers/mv_pp2_bpool.h"
#include "drivers/mv_pp2_ppio.h"
#include "src/drivers/ppv2/pp2.h"

#include "env/trace/trc_pf.h"

#define REGFILE_VAR_DIR		"/var/"
#define REGFILE_NAME_PREFIX	"nic-pf-"
#define REGFILE_MAX_FILE_NAME	64

/* TODO: These should be removed. The local queue sizes should match the remote
 * management queue sizes, as received during the init sequence.
 */
#define LOCAL_CMD_QUEUE_SIZE	256
#define LOCAL_NOTIFY_QUEUE_SIZE	256

#define REGFILE_VERSION		000002	/* Version Format: XX.XX.XX*/

/* PP2 related definitions */
/* Maximum size of port name */
#define NMP_PPIO_NAME_MAX			20

/* Maximum number of packet processors used by NMP */
#define NMP_PP2_MAX_PKT_PROC			2

/* sysfs path for reading relevant parameters from kernel driver */
#define NMP_PP2_SYSFS_MUSDK_PATH		"/sys/devices/platform/pp2/musdk"
#define NMP_PP2_SYSFS_DEBUG_PORT_SET_FILE	"sysfs_current_port"
#define NMP_PP2_SYSFS_RX_FIRST_RXQ_FILE		"first_rxq"
#define NMP_PP2_SYSFS_RX_NUM_RXQ_FILE		"num_rx_queues"
#define NMP_PP2_SYSFS_TX_NUM_TXQ_FILE		"num_tx_queues"

static u16 used_bpools[NMP_PP2_MAX_PKT_PROC];

/**
 * NIC PF Register File Section
 * ============================
 */

/*
 *	nic_pf_regfile_size
 *
 *	This function calculates the regfile data byte size
 *
 *	@param[in]      nic_pf       - Pointer to the NIC-PF struct which defines the topology
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise (< 0)
 */
static int nic_pf_regfile_size(struct nic_pf *nic_pf)
{
	int size;
	u32 tc_id;
	struct giu_gpio_intc_params *intc;
	struct giu_gpio_outtc_params *outtc;
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	/* Main topology structure size */
	size = sizeof(struct giu_regfile);

	/* Add BM Pool size */
	size += (sizeof(struct giu_queue) * q_top->intcs_params.intc_params->num_inpools);

	/* Add Egress TCs size */
	size += (sizeof(struct giu_tc) * (q_top->intcs_params.num_intcs));

	/* Go trough Egress TCs and calc size of queues */
	for (tc_id = 0; tc_id < (q_top->intcs_params.num_intcs); tc_id++) {
		intc = &(q_top->intcs_params.intc_params[tc_id]);
		size += (sizeof(struct giu_queue) * (intc->num_inqs));
	}

	/* Add Ingress TCs size */
	size += (sizeof(struct giu_tc) * (q_top->outtcs_params.num_outtcs));

	/* Go trough Egress TCs and calc size of queues */
	for (tc_id = 0; tc_id < (q_top->outtcs_params.num_outtcs); tc_id++) {
		outtc = &(q_top->outtcs_params.outtc_params[tc_id]);
		size += (sizeof(struct giu_queue) * (outtc->num_outqs));
	}

	return size;
}


/*
 *	nic_pf_regfile_open
 *
 *	This function opens the regfile
 *
 *	@param[in]      nic_pf       - Pointer to the NIC-PF struct which defines the topology
 *			file_map   - Pointer to Pointer to Regfile mempry map
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise (< 0)
 */
static int nic_pf_regfile_open(struct nic_pf *nic_pf, void **file_map)
{
	int size;
	char file_name[REGFILE_MAX_FILE_NAME];

	/* Concatenate file path */
	snprintf(file_name, sizeof(file_name), "%s%s%d", REGFILE_VAR_DIR, REGFILE_NAME_PREFIX, nic_pf->pf_id);

	/* Configure queue topology in register file */
	size = nic_pf_regfile_size(nic_pf);

	if (size < 0) {
		pr_err("Error: failed to map file %s (size %d)\n", file_name, size);
		return -ENOENT;
	}

	*file_map = regfile_open(file_name, size);
	if (file_map == NULL) {
		pr_err("Error: failed to map file %s\n", file_name);
		return -ENOENT;
	}

	pr_info("Regfile Parameters (Regfile Name: %s, Ver %06d)\n", file_name, REGFILE_VERSION);

	return 0;
}


/*
 *	nic_pf_regfile_close
 *
 *	This function opens the regfile
 *
 *	@param[in]      file_map   - Pointer to Regfile mempry map for closing the file
 *
 */
static void nic_pf_regfile_close(void *file_map)
{
	regfile_close(file_map);
}


/*
 *	nic_pf_config_header_regfile
 *
 *	This function writes the header pf to the regfile
 *
 *	@param[in]	regfile_data - Struct of the regfile data
 *			file_map   - Pointer to Pointer to Regfile mempry map
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise (< 0)
 */
static int nic_pf_config_header_regfile(struct giu_regfile *regfile_data, void **file_map)
{
	/* Set Regfile header configuration */
	memcpy(*file_map, regfile_data, sizeof(struct giu_regfile));
	*file_map += sizeof(struct giu_regfile);

	return 0;
}


/*
 *	get_queue_prod_phys_addr
 *
 *	This function gets the Physical addr (producer) of specific queue from the QNPT table
 *
 *      @param[in]	nic_pf     - Pointer to the NIC-PF struct which defines the topology
 *			hw_q_id    - The real unique Queue index
 *
 *	@retval	addr on success
 */
static void *get_queue_prod_phys_addr(struct nic_pf *nic_pf, int hw_q_id)
{
	void *pf_cfg_base; /* pointer to HW */

	/* Get BAR0 Configuration space base address */
	pf_cfg_base = (struct mqa_qnpt_entry *)nic_pf->map.cfg_map.phys_addr;

	/* Calc Notification table specifi entry  */
	return (pf_cfg_base + PCI_BAR0_MQA_QNPT_BASE) +	(sizeof(struct mqa_qnct_entry) * hw_q_id);
}


/*
 *	get_queue_cons_phys_addr
 *
 *	This function gets the Physical addr (consumer) of specific queue from the QNCT table
 *
 *      @param[in]	nic_pf     - Pointer to the NIC-PF struct which defines the topology
 *			hw_q_id    - The real unique Queue index
 *
 *	@retval	addr on success
 */
static void *get_queue_cons_phys_addr(struct nic_pf *nic_pf, int hw_q_id)
{
	void *pf_cfg_base;

	/* Get BAR0 Configuration space base address */
	pf_cfg_base = (struct mqa_qnct_entry *)nic_pf->map.cfg_map.phys_addr;

	/* Calc Notification table specifi entry  */
	return (pf_cfg_base + PCI_BAR0_MQA_QNCT_BASE) +	(sizeof(struct mqa_qnct_entry) * hw_q_id);
}


/*
 *	nic_pf_regfile_register_queue
 *
 *	This function register Queue params in Regfile Queue structure
 *	It gets the info from the SNIC-DB and finally update directly the regfile
 *
 *      @param[in]	nic_pf     - Pointer to the NIC-PF struct which defines the topology
 *			hw_q_id    - The real unique Queue index
 *			q_type     - The type of the Queue (Egress / Ingress / BM/...)
 *			file_map   - Pointer to Pointer to Regfile mempry map
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise (< 0)
 */
static int nic_pf_regfile_register_queue(struct nic_pf *nic_pf,
					union giu_gpio_q_params *giu_gpio_q_p,
					int q_type, void **file_map)
{
	struct giu_queue reg_giu_queue;

	if (giu_gpio_q_p == NULL) {
		pr_err("Failed to get queue params from DB (Queue: %d)\n", (int)giu_gpio_q_p->lcl_q.q_id);
		return -ENODEV;
	}

	reg_giu_queue.hw_id		= giu_gpio_q_p->lcl_q.q_id;
	/** TODO - change params naming - change reg_giu_queue.size to reg_giu_queue.len*/
	reg_giu_queue.size		= giu_gpio_q_p->lcl_q.len;
	reg_giu_queue.type		= q_type;
	reg_giu_queue.phy_base_addr	= giu_gpio_q_p->lcl_q.q->phy_base_addr;
	/* Prod/Cons addr are Virtual. Needs to translate them to Phys addr */
	reg_giu_queue.prod_addr	= get_queue_prod_phys_addr(nic_pf, giu_gpio_q_p->lcl_q.q_id);
	reg_giu_queue.cons_addr	= get_queue_cons_phys_addr(nic_pf, giu_gpio_q_p->lcl_q.q_id);

	/* Note: buff_size & payload_offset are union and they are set
	 *	 acoording to the Q type.
	 */
	if (q_type == QUEUE_BP)
		/** TODO - change params naming - change buff_size to buff_len */
		reg_giu_queue.buff_len = nic_pf->profile_data.lcl_bm_buf_size;
	else
		reg_giu_queue.payload_offset = 0; /* TODO: this should not be hardcoded */

	/* Copy Queues parameters */
	pr_debug("\t\tCopy Queue %d Information to Regfile\n", reg_giu_queue.hw_id);
	memcpy(*file_map, &reg_giu_queue, sizeof(struct giu_queue));
	*file_map += sizeof(struct giu_queue);

	return 0;
}


/*
 *	nic_pf_regfile_register_intc
 *
 *	This function configures In TC params in register file TC structure
 *
 *      @param[in]	nic_pf - Pointer to the NIC-PF struct which defines the topology
 *			intc_params    - Pointer to In TC format of the giu gpio queue topology
 *			tc_queue_type  - Type of the Queues in this specific TC
 *			file_map       - Pointer to Pointer to Regfile mempry map
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise (< 0)
 */
static int nic_pf_regfile_register_intc(struct nic_pf *nic_pf,
					struct giu_gpio_intc_params *intc_params,
					int tc_queue_type, void **file_map)
{
	int queue_idx, ret = 0;
	struct giu_tc reg_giu_tc;

	reg_giu_tc.id			= intc_params->tc_id;
	reg_giu_tc.ingress_rss_type	= 0/*intc_params->rss_type*/;
	reg_giu_tc.num_queues		= intc_params->num_inqs;
	reg_giu_tc.queues		= NULL;
	reg_giu_tc.dest_num_queues	= 0;

	/* Copy TC parameters */
	pr_debug("\tCopy TC %d Information to Regfile\n", reg_giu_tc.id);
	memcpy(*file_map, &reg_giu_tc, sizeof(struct giu_tc));
	*file_map += sizeof(struct giu_tc);

	if (intc_params->inqs_params != NULL) {
		for (queue_idx = 0; queue_idx < reg_giu_tc.num_queues; queue_idx++) {
			union giu_gpio_q_params *hw_q_id = &(intc_params->inqs_params[queue_idx]);

			ret = nic_pf_regfile_register_queue(nic_pf, hw_q_id, tc_queue_type, file_map);

			if (ret != 0)
				break;
		}
	} else {
		pr_info("Topology Queue list in TC is empty (NULL)\n");
	}

	return ret;

}


/*
 *	nic_pf_regfile_register_outtc
 *
 *	This function configures Out TC params in register file TC structure
 *
 *      @param[in]	nic_pf - Pointer to the NIC-PF struct which defines the topology
 *			outtc_params   - Pointer to Out TC format of the giu gpio queue topology
 *			tc_queue_type  - Type of the Queues in this specific TC
 *			file_map       - Pointer to Pointer to Regfile mempry map
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise (< 0)
 */
static int nic_pf_regfile_register_outtc(struct nic_pf *nic_pf,
					struct giu_gpio_outtc_params *outtc_params,
					int tc_queue_type, void **file_map)
{
	int queue_idx, ret = 0;
	struct giu_tc reg_giu_tc;

	reg_giu_tc.id			= outtc_params->tc_id;
	reg_giu_tc.ingress_rss_type	= outtc_params->rss_type;
	reg_giu_tc.num_queues		= outtc_params->num_outqs;
	reg_giu_tc.queues		= NULL;
	reg_giu_tc.dest_num_queues	= outtc_params->num_rem_inqs;

	/* Copy TC parameters */
	pr_debug("\tCopy TC %d Information to Regfile\n", reg_giu_tc.id);
	memcpy(*file_map, &reg_giu_tc, sizeof(struct giu_tc));
	*file_map += sizeof(struct giu_tc);

	if (outtc_params->outqs_params != NULL) {
		for (queue_idx = 0; queue_idx < reg_giu_tc.num_queues; queue_idx++) {
			union giu_gpio_q_params *hw_q_id = &(outtc_params->outqs_params[queue_idx]);

			ret = nic_pf_regfile_register_queue(nic_pf, hw_q_id, tc_queue_type, file_map);

			if (ret != 0)
				break;
		}
	} else {
		pr_info("Topology Queue list in TC is empty (NULL)\n");
	}

	return ret;

}


/*
 *	nic_pf_config_topology_and_update_regfile
 *
 *	This function configures the entite NIC-PF struct params to the register file
 *      It runs over all TCs types (Egress / Ingress / ...) and BM Queues structure and convert the
 *      Topology to the register file
 *
 *	@param[in]	nic_pf      - Pointer to the NIC-PF struct which defines the topology
 *+
 *	@retval	0 on success
 *	@retval	error-code otherwise (< 0)
 */

static int nic_pf_config_topology_and_update_regfile(struct nic_pf *nic_pf)
{
	void *file_map;
	struct giu_regfile *regfile_data = &nic_pf->regfile_data;
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);
	int tc_idx, queue_idx;
	void *pf_cfg_base;
	int ret = 0;
	int bm_tc_id = 0;

	/* Update Regfile general info */
	regfile_data->version		= REGFILE_VERSION;
	regfile_data->num_bm_qs		= q_top->intcs_params.intc_params->num_inpools;
	regfile_data->bm_qs		= NULL;
	regfile_data->num_egress_tcs	= q_top->intcs_params.num_intcs;
	regfile_data->egress_tcs	= NULL;
	regfile_data->num_ingress_tcs	= q_top->outtcs_params.num_outtcs;
	regfile_data->ingress_tcs	= NULL;

	pf_cfg_base = nic_pf->map.cfg_map.virt_addr;
	regfile_data->prod_tbl_base_phys = get_queue_prod_phys_addr(nic_pf, 0);
	regfile_data->prod_tbl_base_virt = (void *)(pf_cfg_base + PCI_BAR0_MQA_QNPT_BASE);
	regfile_data->cons_tbl_base_phys = get_queue_cons_phys_addr(nic_pf, 0);
	regfile_data->cons_tbl_base_virt = (void *)(pf_cfg_base + PCI_BAR0_MQA_QNCT_BASE);

	pr_debug("PCI tables addr: pp: %p, pv: %p, cp: %p, cv: %p.\n",
			regfile_data->prod_tbl_base_phys, regfile_data->prod_tbl_base_virt,
			regfile_data->cons_tbl_base_phys, regfile_data->cons_tbl_base_virt);

	pr_debug("Start Topology configuration to register file [Regfile ver (%d), NIC-PF number (%d)]\n",
			regfile_data->version, nic_pf->pf_id);

	ret = nic_pf_regfile_open(nic_pf, &file_map);

	if (ret != 0)
		return ret;

	/* Copy Header File parameters */
	pr_debug("Copy Regfile Header\n");
	ret = nic_pf_config_header_regfile(regfile_data, &file_map);

	if (ret != 0)
		goto config_error;

	/* Setup BM Queues  */
	if (q_top->intcs_params.intc_params[bm_tc_id].pools != NULL) {
		for (queue_idx = 0; queue_idx < regfile_data->num_bm_qs; queue_idx++) {

			union giu_gpio_q_params *hw_q_id =
						&(q_top->intcs_params.intc_params[bm_tc_id].pools[queue_idx]);

			ret = nic_pf_regfile_register_queue(nic_pf, hw_q_id, QUEUE_BP, &file_map);

			if (ret != 0)
				goto config_error;

		}
	} else {
		pr_info("Topology BM Queue list is empty (NULL)\n");
	}

	/* Setup Egress TCs  */
	if (q_top->intcs_params.intc_params != NULL) {
		for (tc_idx = 0; tc_idx < regfile_data->num_egress_tcs; tc_idx++) {

			struct giu_gpio_intc_params *intc_params = &(q_top->intcs_params.intc_params[tc_idx]);

			ret = nic_pf_regfile_register_intc(nic_pf, intc_params, QUEUE_EGRESS, &file_map);

			if (ret != 0)
				goto config_error;

		}
	} else {
		pr_info("Egress TCs Topology is empty (NULL)\n");
	}


	/* Setup Ingress TCs  */
	if (q_top->outtcs_params.outtc_params != NULL) {
		for (tc_idx = 0; tc_idx < regfile_data->num_ingress_tcs; tc_idx++) {

			struct giu_gpio_outtc_params *outtc_params = &(q_top->outtcs_params.outtc_params[tc_idx]);

			ret = nic_pf_regfile_register_outtc(nic_pf, outtc_params, QUEUE_INGRESS, &file_map);

			if (ret != 0)
				goto config_error;

		}
	} else {
		pr_info("Ingress TCs Topology is empty (NULL)\n");
	}

	return ret;

config_error:

	nic_pf_regfile_close(file_map);

	return ret;
}


/**
 * NIC PF Initialization Section
 * =============================
 */

/*
 *	nic_pf_topology_local_queue_init
 *
 *	This function initialize local queues in NIC PF queue
 *	topology database based on configuration profile
 *	Local queues include data queues and bm queues
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_topology_local_queue_init(struct nic_pf *nic_pf)
{
	int ret;
	struct pf_profile *prof = &(nic_pf->profile_data);
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	pr_debug("Initializing Local Queues in management Database\n");

	/* Local Egress TC */
	ret = pf_intc_queue_init(LCL, q_top->intcs_params.num_intcs, prof->lcl_egress_q_num);
	if (ret) {
		pr_err("Failed to allocate Local Egress TC table\n");
		goto queue_error;
	}

	/* Local ingress TC */
	ret = pf_outtc_queue_init(LCL, q_top->outtcs_params.num_outtcs, prof->lcl_ingress_q_num);
	if (ret) {
		pr_err("Failed to allocate Local Ingress TC table\n");
		goto queue_error;
	}

	/* Local BM */
	ret = pf_intc_bm_queue_init(prof->lcl_bm_q_num);
	if (ret) {
		pr_err("Failed to allocate Local BM table\n");
		goto queue_error;
	}

	return 0;

queue_error:

	pf_intc_queue_free(LCL, q_top->intcs_params.num_intcs);
	pf_outtc_queue_free(LCL, q_top->outtcs_params.num_outtcs);
	pf_intc_bm_queue_free();

	return -ENOMEM;
}


/*
 *	nic_pf_topology_local_tc_free
 *
 *	This function frees the local TC resources in topology
 */
static void nic_pf_topology_local_tc_free(struct nic_pf *nic_pf)
{
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	pr_debug("Free Local queues DB\n");
	pf_intc_queue_free(LCL, q_top->intcs_params.num_intcs);
	pf_outtc_queue_free(LCL, q_top->outtcs_params.num_outtcs);
	pf_intc_bm_queue_free();
}


/*
 *	nic_pf_topology_remote_queue_init
 *
 *	This function initialize NIC PF remote queue
 *	topology database based on PF_INIT management command
 *	Remote queues include data queues and bm queues
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_topology_remote_queue_init(struct nic_pf *nic_pf)
{
	int ret;
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	pr_debug("Initializing Remote Queues in management Database\n");

	/* Remote Egress TC */
	/* TC queues will be update upon "tc_add" command */
	ret = pf_intc_queue_init(REM, q_top->intcs_params.num_intcs, 0);
	if (ret) {
		pr_err("Failed to allocate Local Egress TC table\n");
		goto queue_error;
	}

	/* Remote ingress TC */
	/* TC queues will be update upon "tc_add" command */
	ret = pf_outtc_queue_init(REM, q_top->outtcs_params.num_outtcs, 0);
	if (ret) {
		pr_err("Failed to allocate Local Ingress TC table\n");
		goto queue_error;
	}

	return 0;

queue_error:

	pr_err("Remote Queues Initialization failed\n");

	pf_intc_queue_free(REM, q_top->intcs_params.num_intcs);
	pf_outtc_queue_free(REM, q_top->outtcs_params.num_outtcs);

	return -ENOMEM;
}


/*
 *	nic_pf_topology_remote_tc_free
 *
 *	This function frees the remote TC resources in topology
 */
static int nic_pf_topology_remote_tc_free(struct nic_pf *nic_pf)
{
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	pr_debug("Free Remote queues DB\n");
	pf_intc_queue_free(REM, q_top->intcs_params.num_intcs);
	pf_outtc_queue_free(REM, q_top->outtcs_params.num_outtcs);

	return 0;
}


/*
 *	nic_pf_topology_tc_free
 *
 *	This function frees both local & remote TC resources in DB
 */
static int nic_pf_topology_tc_free(struct nic_pf *nic_pf)
{
	/* Free Local TC structures */
	nic_pf_topology_local_tc_free(nic_pf);

	/* Free Remote TC structures */
	nic_pf_topology_remote_tc_free(nic_pf);

	return 0;
}


/*
 *	nic_pf_topology_local_queue_cfg
 *
 *	This function create NIC PF local queue based on configuration profile
 *	Local queues include data queues and bm queues
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_topology_local_queue_cfg(struct nic_pf *nic_pf)
{
	int ret;
	u32 tc_idx;
	u32 bm_idx;
	u32 q_idx;
	u32 q_id;

	struct giu_gpio_outtc_params *outtc;
	struct giu_gpio_intc_params *intc;
	union  giu_gpio_q_params giu_gpio_q;
	union  giu_gpio_q_params *giu_gpio_q_p;
	struct pf_profile *prof = &(nic_pf->profile_data);
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	/* Create Local BM queues */
	pr_debug("Configure Local BM queues (Num of queues %d)\n", q_top->intcs_params.num_inpools);

	for (tc_idx = 0; tc_idx < q_top->intcs_params.num_intcs; tc_idx++) {

		intc = &(q_top->intcs_params.intc_params[tc_idx]);

		for (bm_idx = 0; bm_idx < intc->num_inpools; bm_idx++) {

			/* Clear queue structure */
			memset(&giu_gpio_q, 0, sizeof(union giu_gpio_q_params));

			/* Allocate queue from MQA */
			ret = mqa_queue_alloc(nic_pf->mqa, &q_id);
			if (ret < 0) {
				pr_err("Failed to allocate queue from MQA\n");
				goto lcl_bm_queue_error;
			}

			/* Init queue parameters */
			giu_gpio_q.lcl_q.q_id = q_id;
			giu_gpio_q.lcl_q.len = prof->lcl_bm_q_size;

			/* Save queue info */
			memcpy(&(intc->pools[bm_idx]), &(giu_gpio_q), sizeof(union giu_gpio_q_params));

			pr_debug("Configure BM[%d] TC[%d], Id %d\n\n",
					tc_idx, bm_idx, intc->pools[bm_idx].lcl_q.q_id);
		}

		intc->pool_buf_size = prof->lcl_bm_buf_size;
	}


	/* Create Local Egress TC queues */
	pr_debug("Configure Local Egress TC queues (Num of queues %d)\n", q_top->intcs_params.num_intcs);

	for (tc_idx = 0; tc_idx < q_top->intcs_params.num_intcs; tc_idx++) {

		intc = &(q_top->intcs_params.intc_params[tc_idx]);

		for (q_idx = 0; q_idx < intc->num_inqs; q_idx++) {

			/* Clear queue structure */
			memset(&giu_gpio_q, 0, sizeof(union giu_gpio_q_params));

			/* Allocate queue from MQA */
			ret = mqa_queue_alloc(nic_pf->mqa, &q_id);
			if (ret < 0) {
				pr_err("Failed to allocate queue from MQA\n");
				goto lcl_eg_queue_error;
			}

			giu_gpio_q.lcl_q.q_id = q_id;
			giu_gpio_q.lcl_q.len  = prof->lcl_egress_q_size;

			/* Save queue info */
			memcpy(&(intc->inqs_params[q_idx]), &(giu_gpio_q), sizeof(union giu_gpio_q_params));

			pr_debug("Configure Egress TC[%d], queue[%d] = Id %d\n\n",
					tc_idx, q_idx, intc->inqs_params[q_idx].lcl_q.q_id);
		}
	}


	/* Create Local Ingress TC queues */
	pr_debug("Configure Local Ingress TC queues (Num of queues %d)\n", q_top->outtcs_params.num_outtcs);

	for (tc_idx = 0; tc_idx < q_top->outtcs_params.num_outtcs; tc_idx++) {

		outtc = &(q_top->outtcs_params.outtc_params[tc_idx]);

		for (q_idx = 0; q_idx < outtc->num_outqs; q_idx++) {

			/* Clear queue structure */
			memset(&giu_gpio_q, 0, sizeof(union giu_gpio_q_params));

			/* Allocate queue from MQA */
			ret = mqa_queue_alloc(nic_pf->mqa, &q_id);
			if (ret < 0) {
				pr_err("Failed to allocate queue from MQA\n");
				goto lcl_ing_queue_error;
			}

			giu_gpio_q.lcl_q.q_id = q_id;
			giu_gpio_q.lcl_q.len  = prof->lcl_ingress_q_size;

			/* Save queue info */
			memcpy(&(outtc->outqs_params[q_idx]), &(giu_gpio_q), sizeof(union giu_gpio_q_params));

			pr_debug("Configure Ingress TC[%d], queue[%d] = Id %d\n\n",
					tc_idx, q_idx, outtc->outqs_params[q_idx].lcl_q.q_id);
		}
	}

	return 0;

lcl_ing_queue_error:

	for (tc_idx = 0; tc_idx < q_top->outtcs_params.num_outtcs; tc_idx++) {
		outtc = &(q_top->outtcs_params.outtc_params[tc_idx]);

		for (q_idx = 0; q_idx < outtc->num_outqs; q_idx++) {
			giu_gpio_q_p = &(outtc->outqs_params[q_idx]);
			if (giu_gpio_q_p != NULL) {

				ret = mqa_queue_free(nic_pf->mqa, giu_gpio_q_p->lcl_q.q_id);
				if (ret)
					pr_err("Failed to free queue Idx %x in MQA\n", giu_gpio_q_p->lcl_q.q_id);

				memset(&(giu_gpio_q_p), 0, sizeof(union giu_gpio_q_params));
			}
		}
	}

lcl_eg_queue_error:

	for (tc_idx = 0; tc_idx < q_top->intcs_params.num_intcs; tc_idx++) {
		intc = &(q_top->intcs_params.intc_params[tc_idx]);

		for (q_idx = 0; q_idx < intc->num_inqs; q_idx++) {
			giu_gpio_q_p = &(intc->inqs_params[q_idx]);
			if (giu_gpio_q_p != NULL) {

				ret = mqa_queue_free(nic_pf->mqa, (u32)giu_gpio_q_p->lcl_q.q_id);
				if (ret)
					pr_err("Failed to free queue Idx %x in MQA\n", giu_gpio_q_p->lcl_q.q_id);

				memset(&(giu_gpio_q_p), 0, sizeof(union giu_gpio_q_params));
			}
		}
	}

lcl_bm_queue_error:

	for (bm_idx = 0; bm_idx < q_top->intcs_params.intc_params->num_inpools; bm_idx++) {
		giu_gpio_q_p = &(q_top->intcs_params.intc_params->pools[bm_idx]);
		if (giu_gpio_q_p != NULL) {

			ret = mqa_queue_free(nic_pf->mqa, (u32)giu_gpio_q_p->lcl_q.q_id);
			if (ret)
				pr_err("Failed to free queue Idx %x in MQA\n", giu_gpio_q_p->lcl_q.q_id);

			memset(&(giu_gpio_q_p), 0, sizeof(union giu_gpio_q_params));
		}
	}

	return -1;
}


/*
 *	nic_pf_mng_chn_init
 *
 *	This function create NIC PF management channel
 *	Execution requires handshake with Host side
 *
 *  The creation flow is:
 *  - Create Local Qs
 *  - Wait for the Host to indicate 'Host Management Ready'
 *  - Register Host Command Q (and set Producer index in BAR 0)
 *  - Register Host Notification Q (and set Consumer index in BAR 0)
 *  - Associate Local Command Q with Host Q: Host (src) --> Local (Dest)
 *  - Associate Local Notification Q with Host Q: Local (src) --> Host (Dest)
 *  - Set 'Device Management Ready' indication
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_mng_chn_init(struct nic_pf *nic_pf)
{
	volatile struct pcie_config_mem *pcie_cfg;
	u64 pf_cfg_phys, pf_cfg_virt; /* pointer to HW so it should be volatile */
	void *qnpt_phys, *qnct_phys;
	u32 local_cmd_queue, local_notify_queue;
	u32 remote_cmd_queue, remote_notify_queue;
	int ret = 0;

	struct mqa_queue_params params;
	struct mqa_q *lcl_cmd_queue_p    = NULL;
	struct mqa_q *lcl_notify_queue_p = NULL;
	struct mqa_q *rem_cmd_queue_p    = NULL;
	struct mqa_q *rem_notify_queue_p = NULL;

	/*  Create Local Queues */
	/* ==================== */

	/* Allocate and Register Local Command queue in MQA */
	pr_info("Register Local Command Q\n");

	/* Allocate queue from MQA */
	ret = mqa_queue_alloc(nic_pf->mqa, &local_cmd_queue);
	if (ret < 0) {
		pr_err("Failed to allocate queue from MQA\n");
		goto exit_error;
	}

	memset(&params, 0, sizeof(struct mqa_queue_params));

	params.idx  = local_cmd_queue;
	params.len  = LOCAL_CMD_QUEUE_SIZE;
	params.size = sizeof(struct cmd_desc);
	params.attr = LOCAL_QUEUE | EGRESS_QUEUE;
	params.prio = 0;

	ret = mqa_queue_create(nic_pf->mqa, &(params), &(lcl_cmd_queue_p));
	if (ret < 0) {
		pr_info("Failed to register Host Management Q\n");
		goto exit_error;
	}

	nic_pf->mng_data.lcl_mng_ctrl.cmd_queue = (struct mqa_q *)lcl_cmd_queue_p;

	/* Allocate and Register Local Notification queue in MQA */
	pr_info("Register Local Notification Q\n");

	/* Allocate queue from MQA */
	ret = mqa_queue_alloc(nic_pf->mqa, &local_notify_queue);
	if (ret < 0) {
		pr_err("Failed to allocate queue from MQA\n");
		goto exit_error;
	}
	memset(&params, 0, sizeof(struct mqa_queue_params));

	params.idx   = local_notify_queue;
	params.len   = LOCAL_NOTIFY_QUEUE_SIZE;
	params.size  = sizeof(struct notif_desc);
	params.attr  = LOCAL_QUEUE | INGRESS_QUEUE;
	params.prio  = 0;

	ret = mqa_queue_create(nic_pf->mqa, &(params), &(lcl_notify_queue_p));
	if (ret < 0) {
		pr_info("Failed to register Host Management Q\n");
		goto exit_error;
	}

	nic_pf->mng_data.lcl_mng_ctrl.notify_queue = (struct mqa_q *)lcl_notify_queue_p;

	/*  Host Ready Check */
	/* ================= */

	/* Get BAR0 Configuration space base address */
	pf_cfg_phys = (u64)nic_pf->map.cfg_map.phys_addr;
	pf_cfg_virt = (u64)nic_pf->map.cfg_map.virt_addr;
	pcie_cfg    = (void *)(pf_cfg_virt + PCI_BAR0_MNG_CH_BASE);

	/* Calc Notification tables base */
	qnct_phys = (void *)(pf_cfg_phys + PCI_BAR0_MQA_QNCT_BASE);
	qnpt_phys = (void *)(pf_cfg_phys + PCI_BAR0_MQA_QNPT_BASE);

	/* Wait for Host to update the state to 'Host Management Ready'
	 * This means that BAR 0 configuration can be accessed as the
	 * Host updated the relevant data/fields.
	 */
	pr_info("Wait till Host change the status to 'Host Management Ready'\n");

	/* TODO - get the mac address from somewhere that makes sense */
	pcie_cfg->mac_addr[0] = 0x0;
	pcie_cfg->mac_addr[1] = 0x1;
	pcie_cfg->mac_addr[2] = 0x2;
	pcie_cfg->mac_addr[3] = 0x3;
	pcie_cfg->mac_addr[4] = 0x4;
	pcie_cfg->mac_addr[5] = 0x5;

	/*
	 * Notification Tables Info:
	 * TODO: Move the Mac address and notification tables configuration to
	 * a separate function, as they are not really related to nic-pf
	 * mng-channel creation.
	 */
	pcie_cfg->prod_notif_tbl_offset = PCI_BAR0_MQA_QNPT_BASE;
	pcie_cfg->prod_notif_tbl_size   = PCI_BAR0_MQA_QNPT_SIZE;
	pcie_cfg->cons_notif_tbl_offset = PCI_BAR0_MQA_QNCT_BASE;
	pcie_cfg->cons_notif_tbl_size   = PCI_BAR0_MQA_QNCT_SIZE;

	/* Make sure that above configuration are out before setting the
	 * dev-ready status for the host side.
	 */
	wmb();

	pcie_cfg->status = PCIE_CFG_STATUS_DEV_READY;

	while (!(pcie_cfg->status & PCIE_CFG_STATUS_HOST_MGMT_READY))
		; /* Do Nothing. Wait till state it's updated */

	pr_info("Host is Ready\n");

	/*  Register Remote Queues */
	/* ======================= */

	/* Register Host Command management queue */
	pr_info("Register host command queue\n");

	/* Allocate queue from MQA */
	ret = mqa_queue_alloc(nic_pf->mqa, &remote_cmd_queue);
	if (ret < 0) {
		pr_err("Failed to allocate queue from MQA\n");
		goto exit_error;
	}

	memset(&params, 0, sizeof(struct mqa_queue_params));

	params.idx             = remote_cmd_queue;
	params.len             = pcie_cfg->cmd_q.len;
	params.size            = sizeof(struct cmd_desc);
	params.attr            = REMOTE_QUEUE | EGRESS_QUEUE;
	params.prio            = 0;
	params.remote_phy_addr = (void *)pcie_cfg->cmd_q.q_addr;
	params.cons_phys       = (void *)(pcie_cfg->cmd_q.consumer_idx_addr + nic_pf->map.host_map.phys_addr);
	params.cons_virt       = (void *)(pcie_cfg->cmd_q.consumer_idx_addr + nic_pf->map.host_map.virt_addr);
	params.host_remap      = nic_pf->map.host_map.phys_addr;
	params.peer_id         = local_cmd_queue;

	/* Allocate queue from MQA */
	ret = mqa_queue_create(nic_pf->mqa, &(params), &(rem_cmd_queue_p));
	if (ret < 0) {
		pr_err("Failed to register Host Management Q\n");
		goto exit_error;
	}

	/* Update PCI BAR0 with producer address (Entry index in notification table) */
	pcie_cfg->cmd_q.producer_idx_addr = (u64)(rem_cmd_queue_p->prod_phys - qnpt_phys) / sizeof(u32);

	nic_pf->mng_data.host_mng_ctrl.cmd_queue = (struct mqa_q *)rem_cmd_queue_p;


	/* Register Host Notification queue */
	pr_info("Register host notification queue\n");

	/* Allocate queue from MQA */
	ret = mqa_queue_alloc(nic_pf->mqa, &remote_notify_queue);
	if (ret < 0) {
		pr_err("Failed to allocate queue from MQA\n");
		goto exit_error;
	}

	memset(&params, 0, sizeof(struct mqa_queue_params));

	params.idx             = remote_notify_queue;
	params.len             = pcie_cfg->notif_q.len;
	params.size            = sizeof(struct notif_desc);
	params.attr            = REMOTE_QUEUE | INGRESS_QUEUE;
	params.prio            = 0;
	params.remote_phy_addr = (void *)pcie_cfg->notif_q.q_addr;
	params.prod_phys       = (void *)(pcie_cfg->notif_q.producer_idx_addr + nic_pf->map.host_map.phys_addr);
	params.prod_virt       = (void *)(pcie_cfg->notif_q.producer_idx_addr + nic_pf->map.host_map.virt_addr);
	params.host_remap      = nic_pf->map.host_map.phys_addr;

	ret = mqa_queue_create(nic_pf->mqa, &(params), &(rem_notify_queue_p));
	if (ret < 0) {
		pr_err("Failed to register Host Management Q\n");
		goto exit_error;
	}

	ret = mqa_queue_associate_pair(nic_pf->mqa, local_notify_queue, remote_notify_queue);
	if (ret < 0) {
		pr_err("Failed to associate Notification queues (Src %d Dest %d)\n",
				local_notify_queue, remote_notify_queue);
		goto exit_error;
	}

	/* Update PCI BAR0 with consumer address (Entry index in notification table) */
	pcie_cfg->notif_q.consumer_idx_addr = (u64)(rem_notify_queue_p->cons_phys - qnct_phys) / sizeof(u32);

	nic_pf->mng_data.host_mng_ctrl.notify_queue = (struct mqa_q *)rem_notify_queue_p;

	/* Register Qs in GIU */
	/* ================== */

	/* Register Command channel */
	gie_add_queue(nic_pf->gie.mng_gie, remote_cmd_queue, 1);

	/* Register Notification channel */
	gie_add_queue(nic_pf->gie.mng_gie, local_notify_queue, 0);


	/* Device Ready */
	/* ============ */

	/* make sure all writes are done before updating the status */
	wmb();

	/* Set state to 'Device Management Ready' */
	pr_info("Set status to 'Device Management Ready'\n");
	pcie_cfg->status |= PCIE_CFG_STATUS_DEV_MGMT_READY;

	return 0;

exit_error:

	if (local_cmd_queue >= 0) {
		if (lcl_cmd_queue_p) {
			ret = mqa_queue_destroy(nic_pf->mqa, lcl_cmd_queue_p);
			if (ret < 0)
				pr_err("Failed to free Local Cmd Q %d in DB\n", local_cmd_queue);
			kfree(lcl_cmd_queue_p);
		}
		ret = mqa_queue_free(nic_pf->mqa, local_cmd_queue);
		if (ret < 0)
			pr_err("Failed to free Local Cmd Q %d in MQA\n", local_cmd_queue);
	}

	if (local_notify_queue >= 0) {
		if (lcl_notify_queue_p) {
			ret = mqa_queue_destroy(nic_pf->mqa, lcl_notify_queue_p);
			if (ret < 0)
				pr_err("Failed to free Local Notify Q %d in DB\n", local_notify_queue);
			kfree(lcl_notify_queue_p);
		}
		ret = mqa_queue_free(nic_pf->mqa, local_notify_queue);
		if (ret < 0)
			pr_err("Failed to free Local Notify Q %d in MQA\n", local_notify_queue);
	}

	if (remote_cmd_queue >= 0) {
		if (rem_cmd_queue_p) {
			ret = mqa_queue_destroy(nic_pf->mqa, rem_cmd_queue_p);
			if (ret < 0)
				pr_err("Failed to free remote Cmd Q %d in DB\n", remote_cmd_queue);
			kfree(rem_cmd_queue_p);
		}
		ret = mqa_queue_free(nic_pf->mqa, remote_cmd_queue);
		if (ret < 0)
			pr_err("Failed to free remote Cmd Q %d in MQA\n", remote_cmd_queue);
	}

	if (remote_notify_queue >= 0) {
		if (rem_notify_queue_p) {
			ret = mqa_queue_destroy(nic_pf->mqa, rem_notify_queue_p);
			if (ret < 0)
				pr_err("Failed to free Remote Notify Q %d in DB\n", remote_notify_queue);
			kfree(rem_notify_queue_p);
		}
		ret = mqa_queue_free(nic_pf->mqa, remote_notify_queue);
		if (ret < 0)
			pr_err("Failed to free Remote Notify Q %d in MQA\n", remote_notify_queue);
	}

	return ret;
}

/*
 *	nic_pf_pp2_port_pp2_init
 *
 *	Initialize the pp2_port of type PP2
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_pp2_port_pp2_init(struct nic_pf *nic_pf)
{
	int					err;
	int					k;
	struct nmp_pp2_bpool_desc		*pools = NULL;
	u32					 pp_id, port_id, scanned;
	struct nmp_lf_nicpf_pp2_port_params	*pf_pp2_profile;
	struct nmp_pp2_port_desc		*pdesc;

	pf_pp2_profile = &nic_pf->profile_data.pp2_port;
	nic_pf->pp2.num_ports = 1;
	nic_pf->pp2.reserved_bpools = nic_pf->profile_data.pp2_bm_pool_reserved_map;

	/* Allocate memory for the pp2 descriptors */
	pdesc = kmalloc(sizeof(struct nmp_pp2_port_desc), GFP_KERNEL);
	if (!pdesc)
		return -ENOMEM;

	nic_pf->pp2.ports_desc = pdesc;
	pdesc->num_pools = pf_pp2_profile->lcl_num_bpools;
	pr_info("Number of pools %d\n", pdesc->num_pools);
	pools = kmalloc((sizeof(struct nmp_pp2_bpool_desc) * pdesc->num_pools), GFP_KERNEL);
	if (!pools) {
		pr_err("no mem for bpool_desc array!\n");
		err =  -ENOMEM;
		goto init_desc_exit1;
	}
	pdesc->pools_desc = pools;

	/* Parse match string to ring number */
	scanned = sscanf(pf_pp2_profile->match, "ppio-%d:%d\n", &pp_id, &port_id);
	if (scanned != 2) {
		pr_err("Invalid match string %s. Expected: ppio-0:X\n",
			pf_pp2_profile->match);
		err =  -EINVAL;
		goto init_desc_exit2;
	}
	pdesc->pp_id = pp_id;
	pdesc->ppio_id = port_id;

	pr_debug("pp_id %d, port_id %d\n", pdesc->pp_id, pdesc->ppio_id);
	for (k = 0; k < pdesc->num_pools; k++) {
		pools[k].num_buffs = pf_pp2_profile->lcl_bpools_params[k].max_num_buffs;
		pools[k].buff_size = pf_pp2_profile->lcl_bpools_params[k].buff_size;
	}
	pdesc->first_inq = 0; /* Fixed value */
	pdesc->first_rss_tbl = 0; /* Fixed value */
	pdesc->hash_type = 0; /* Fixed value */
	pdesc->pkt_offst = nic_pf->profile_data.dflt_pkt_offset;
	pr_info("pdesc pkt_offset: %d\n", pdesc->pkt_offst);
	pdesc->inq_size = nic_pf->profile_data.lcl_ingress_q_size;
	pdesc->max_num_tcs = nic_pf->profile_data.max_num_tcs;
	pdesc->num_tcs = 1; /* Value is updated after init_done command */
	for (k = 0; k < pdesc->num_tcs; k++)
		pdesc->num_inqs[k] = 1; /* Value is updated after init_done command */
	pdesc->num_outqs = 1; /* Value is updated after init_done command */
	pdesc->outq_size = nic_pf->profile_data.lcl_egress_q_size;

	return 0;

init_desc_exit2:
	kfree(pools);
init_desc_exit1:
	kfree(pdesc);
	return err;
}

/*
 *	nic_pf_pp2_port_init
 *
 *	Initialize the pp2_port according to port type (PP2 or LAG)
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_pp2_port_init(struct nic_pf *nic_pf)
{
	int					err = 0;

	/* Get the number of ports requested */
	/* TODO: currently only one container and one LF supported. */

	switch (nic_pf->profile_data.port_type) {
	case NMP_LF_NICPF_T_NONE:
		/* no pp2 port, just return */
		return 0;
	case NMP_LF_NICPF_T_PP2_PORT:
		err = nic_pf_pp2_port_pp2_init(nic_pf);
		break;
	case NMP_LF_NICPF_T_PP2_LAG:
		pr_err("nicpf of type PP2_LAG is not supported yet\n");
		err = -EINVAL;
		break;
	default:
		pr_err("invalid nicpf type\n");
		err = -EINVAL;
	}

	return err;
}

/*
 *	nic_pf_init
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
int nic_pf_init(struct nic_pf *nic_pf)
{
	int ret;
	struct nmdisp_client_params params;
	struct nmdisp_q_pair_params q_params;

	/* Clear queue topology batabase */
	memset(&(nic_pf->topology_data), 0, sizeof(struct giu_gpio_init_params));

	nic_pf->topology_data.mqa = nic_pf->mqa;
	nic_pf->topology_data.gie = &(nic_pf->gie);

	nic_pf->pf_id = 0;

	/* Initialize management queues */
	ret = nic_pf_mng_chn_init(nic_pf);
	if (ret)
		return ret;

	/* Register NIC PF to dispatcher */
	params.client_type  = CDT_PF;
	params.client_id    = nic_pf->pf_id;
	params.client_sr_cb = nic_pf_process_command;
	params.client       = nic_pf;

	ret = nmdisp_register_client(nic_pf->nmdisp, &params);
	if (ret)
		return ret;

	q_params.cmd_q    = nic_pf->mng_data.lcl_mng_ctrl.cmd_queue;
	q_params.notify_q = nic_pf->mng_data.lcl_mng_ctrl.notify_queue;

	ret = nmdisp_add_queue(nic_pf->nmdisp, params.client_type, params.client_id, &q_params);
	if (ret)
		return ret;

	/* Initialize the nicpf PP2 port */
	nic_pf_pp2_port_init(nic_pf);

	return 0;
}


/**
 * NIC PF Termination Section
 * ==========================
 */

/*
 *	nic_pf_local_queue_terminate
 *
 *	This function terminate NIC PF local queue based on configuration profile
 *	Local queues include data queues and bm queues
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_local_queue_terminate(struct nic_pf *nic_pf)
{
	nic_pf = nic_pf;

	return 0;
}


/*
 *	nic_pf_mng_chn_terminate
 *
 *	This function terminate NIC PF management channel
 *	Execution requires handshake with Host side
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
static int nic_pf_mng_chn_terminate(struct nic_pf *nic_pf)
{
	int local_cmd_queue, local_notify_queue;
	int remote_cmd_queue, remote_notify_queue;
	int ret = 0;

	struct mqa_q *lcl_cmd_queue_p    = nic_pf->mng_data.lcl_mng_ctrl.cmd_queue;
	struct mqa_q *lcl_notify_queue_p = nic_pf->mng_data.lcl_mng_ctrl.notify_queue;
	struct mqa_q *rem_cmd_queue_p    = nic_pf->mng_data.host_mng_ctrl.cmd_queue;
	struct mqa_q *rem_notify_queue_p = nic_pf->mng_data.host_mng_ctrl.notify_queue;

	if (lcl_cmd_queue_p) {
		local_cmd_queue = lcl_cmd_queue_p->q_id;
		ret = mqa_queue_destroy(nic_pf->mqa, lcl_cmd_queue_p);
		if (ret < 0)
			pr_err("Failed to free Local Cmd Q %d in DB\n", local_cmd_queue);
		ret = mqa_queue_free(nic_pf->mqa, local_cmd_queue);
		if (ret < 0)
			pr_err("Failed to free Local Cmd Q %d in MQA\n", local_cmd_queue);

		kfree(lcl_cmd_queue_p);
	}

	if (lcl_notify_queue_p) {
		local_notify_queue = lcl_notify_queue_p->q_id;
		ret = mqa_queue_destroy(nic_pf->mqa, lcl_notify_queue_p);
		if (ret < 0)
			pr_err("Failed to free Local Notify Q %d in DB\n", local_notify_queue);
		ret = mqa_queue_free(nic_pf->mqa, local_notify_queue);
		if (ret < 0)
			pr_err("Failed to free Local Notify Q %d in MQA\n", local_notify_queue);

		kfree(lcl_notify_queue_p);
	}

	if (rem_cmd_queue_p) {
		remote_cmd_queue = rem_cmd_queue_p->q_id;
		ret = mqa_queue_destroy(nic_pf->mqa, rem_cmd_queue_p);
		if (ret < 0)
			pr_err("Failed to free remote Cmd Q %d in DB\n", remote_cmd_queue);
		ret = mqa_queue_free(nic_pf->mqa, remote_cmd_queue);
		if (ret < 0)
			pr_err("Failed to free remote Cmd Q %d in MQA\n", remote_cmd_queue);

		kfree(rem_cmd_queue_p);
	}

	if (rem_notify_queue_p) {
		remote_notify_queue = rem_notify_queue_p->q_id;
		ret = mqa_queue_destroy(nic_pf->mqa, rem_notify_queue_p);
		if (ret < 0)
			pr_err("Failed to free Remote Notify Q %d in DB\n", remote_notify_queue);
		ret = mqa_queue_free(nic_pf->mqa, remote_notify_queue);
		if (ret < 0)
			pr_err("Failed to free Remote Notify Q %d in MQA\n", remote_notify_queue);

		kfree(rem_notify_queue_p);
	}

	return 0;
}


/*
 *	nic_pf_terminate
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
int nic_pf_terminate(struct nic_pf *nic_pf)
{
	int ret;

	ret = nic_pf_local_queue_terminate(nic_pf);
	if (ret)
		return ret;

	ret = nic_pf_mng_chn_terminate(nic_pf);
	if (ret)
		return ret;

	pr_info("Terminating NIC PF\n");
	return 0;
}


/**
 * NIC PF Command Processing Section
 * =================================
 */

/*
 *	nic_pf_gen_resp_msg
 *
 *	This function initialize response message generic parameters
 *	=== Important: Response data is updated at the scope of
 *	nic_pf_process_init_command / nic_pf_process_exec_command APIs
 *
 *	@param[in]	status - command execution result
 *	@param[in]	cmd - pointer to cmd_desc object
 *	@param[out]	resp - pointer to notif_desc object
 *
 *	@retval	none
 */
static void nic_pf_gen_resp_msg(u32 status, struct cmd_desc *cmd,
					struct notif_desc *resp)
{
	resp->cmd_idx  = cmd->cmd_idx;
	resp->app_code = AC_HOST_SNIC_NETDEV;
	resp->status   = (u8)status;
	resp->flags    = 0;

	/* TODO - Add desc / resp parameters size */
	resp->resp_param_size = 0;
	resp->desc_param_size = 0;
}

/*
 *	nic_pf_pf_init_command
 */
static int nic_pf_pf_init_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	int ret, i;

	struct mgmt_cmd_params *params = &(cmd->params);
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	pr_debug("PF INIT\n");
	pr_debug("Num of - Ing TC %d, Eg TC %d\n",
				params->pf_init.num_host_ingress_tc,
				params->pf_init.num_host_egress_tc);

	/* Extract message params and update database */
	q_top->outtcs_params.num_outtcs = params->pf_init.num_host_ingress_tc;
	q_top->intcs_params.num_intcs = params->pf_init.num_host_egress_tc;

	/* Initialize remote queues database */
	ret = nic_pf_topology_remote_queue_init(nic_pf);
	if (ret)
		pr_err("Failed to update remote DB queue info\n");

	/**
	 * NIC PF - PP2 updates
	 */
	/* Update pp2 number of TC's */
	for (i = 0; i < nic_pf->pp2.num_ports; i++)
		nic_pf->pp2.ports_desc[i].num_tcs = params->pf_init.num_host_ingress_tc;

#ifndef GIE_NO_MULTI_Q_SUPPORT_FOR_RSS
	/* Initialize local queues database */
	ret = nic_pf_topology_local_queue_init(nic_pf);
	if (ret) {
		pr_err("Failed to update local DB queue info\n");
		goto pf_init_exit;
	}

	/* Allocate and configure local queues in the database */
	ret = nic_pf_topology_local_queue_cfg(nic_pf);
	if (ret)
		pr_err("Failed to configure PF regfile\n");
pf_init_exit:
#endif

	/* Generate response message */
	nic_pf_gen_resp_msg(ret, cmd, resp);

	pr_debug("PF INIT, Done\n\n");

	return ret;
}


/*
 *	nic_pf_egress_tc_add_command
 */
static int nic_pf_egress_tc_add_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	int ret = 0;
	union giu_gpio_q_params *tc_queues;

	struct mgmt_cmd_params *params = &(cmd->params);
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);
	struct giu_gpio_intc_params *intc = &(q_top->intcs_params.intc_params[params->pf_egress_tc_add.tc_prio]);

	pr_debug("Configure Host Egress TC[%d] Queues\n", params->pf_egress_tc_add.tc_prio);

	tc_queues = kcalloc(params->pf_egress_tc_add.num_queues_per_tc, sizeof(union giu_gpio_q_params), GFP_KERNEL);
	if (tc_queues == NULL) {
		ret = -ENOMEM;
		goto tc_exit;
	}

	/* Update queue topology database */
	intc->rem_outqs_params = tc_queues;
	intc->num_rem_outqs = params->pf_egress_tc_add.num_queues_per_tc;

tc_exit:

	/* Generate response message */
	nic_pf_gen_resp_msg(ret, cmd, resp);

	if (ret) {
		if (tc_queues != NULL)
			kfree(tc_queues);

		pr_err("Host Egress TC[%d] Add failed\n", params->pf_egress_tc_add.tc_prio);
	}

	return ret;
}


/*
 *	nic_pf_egress_tc_add_command
 */
static int nic_pf_ingress_tc_add_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	int ret = 0;

	struct mgmt_cmd_params *params = &(cmd->params);
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);
	struct giu_gpio_outtc_params *outtc = &(q_top->outtcs_params.outtc_params[params->pf_ingress_tc_add.tc_prio]);

	pr_debug("Configure Host Ingress TC[%d] Queues\n", params->pf_ingress_tc_add.tc_prio);

	outtc->rem_inqs_params = kcalloc(params->pf_ingress_tc_add.num_queues_per_tc,
					 sizeof(union giu_gpio_q_params),
					 GFP_KERNEL);
	if (outtc->rem_inqs_params == NULL) {
		ret = -ENOMEM;
		goto tc_exit;
	}

	outtc->rem_poolqs_params = kcalloc(params->pf_ingress_tc_add.num_queues_per_tc,
					   sizeof(union giu_gpio_q_params),
					   GFP_KERNEL);
	if (outtc->rem_poolqs_params == NULL) {
		ret = -ENOMEM;
		goto tc_exit;
	}

	/* Update queue topology database */
	outtc->num_rem_inqs = params->pf_ingress_tc_add.num_queues_per_tc;
	outtc->rss_type = params->pf_ingress_tc_add.hash_type;
	/** TODO - Add support for params->pf_ingress_tc_add.pkt_offset */

tc_exit:

	/* Generate response message */
	nic_pf_gen_resp_msg(ret, cmd, resp);

	if (ret) {
		if (outtc->rem_inqs_params != NULL)
			kfree(outtc->rem_inqs_params);

		if (outtc->rem_poolqs_params != NULL)
			kfree(outtc->rem_poolqs_params);

		pr_err("Host ingress TC[%d] Add failed\n", params->pf_ingress_tc_add.tc_prio);
	}

	return ret;
}


/*
 *	tc_q_next_entry_get
 *
 *	This function return next free queue index in TC queue array
 *	in case no available index return -1
 */
static int tc_q_next_entry_get(union giu_gpio_q_params *q_id_list, u32 q_num)
{
	u32 q_idx;

	for (q_idx = 0; q_idx < q_num; q_idx++) {
		if (q_id_list[q_idx].rem_q.q_id == 0)
			return q_idx;
	}

	return -1;
}


/*
 *	nic_pf_ingress_queue_add_command
 */
static int nic_pf_ingress_queue_add_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	int ret = 0;
	s32 active_q_id;
	u32 msg_tc;
	u32 q_id, bpool_q_id;

	union giu_gpio_q_params giu_gpio_q;
	struct mgmt_cmd_params *params = &(cmd->params);
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);
	struct giu_gpio_outtc_params *outtc;

	msg_tc = params->pf_ingress_data_q_add.tc_prio;
	outtc = &(q_top->outtcs_params.outtc_params[msg_tc]);

	pr_debug("Host Ingress TC[%d], queue Add (num of queues %d)\n", msg_tc, outtc->num_rem_inqs);

	if (nic_pf->profile_data.lcl_bm_buf_size < params->pf_ingress_data_q_add.q_buf_size) {
		pr_err("Host BM buffer size should be at most %d\n", nic_pf->profile_data.lcl_bm_buf_size);
		return -1;
	}

	/* Clear queue structure */
	memset(&giu_gpio_q, 0, sizeof(union giu_gpio_q_params));

	/* Allocate queue from MQA */
	ret = mqa_queue_alloc(nic_pf->mqa, &q_id);
	if (ret < 0) {
		pr_err("Failed to allocate queue from MQA\n");
		return ret;
	}

	/* Init queue parameters */
	giu_gpio_q.rem_q.q_id         = q_id;
	giu_gpio_q.rem_q.len          = params->pf_ingress_data_q_add.q_len;
	giu_gpio_q.rem_q.size         = gie_get_desc_size(RX_DESC);
	giu_gpio_q.rem_q.q_base_pa    = (phys_addr_t)params->pf_ingress_data_q_add.q_phys_addr;
	giu_gpio_q.rem_q.prod_base_pa = (phys_addr_t)(params->pf_ingress_data_q_add.q_prod_phys_addr +
										nic_pf->map.host_map.phys_addr);
	giu_gpio_q.rem_q.prod_base_va = (void *)(params->pf_ingress_data_q_add.q_prod_phys_addr +
										nic_pf->map.host_map.virt_addr);
	giu_gpio_q.rem_q.host_remap   = nic_pf->map.host_map.phys_addr;

	active_q_id = tc_q_next_entry_get(outtc->rem_inqs_params, outtc->num_rem_inqs);
	if (active_q_id < 0) {
		pr_err("Failed to configure queue in Host Ingress TC[%d] queue list\n", msg_tc);
		ret = active_q_id;
		goto ingress_queue_exit;
	}

	pr_debug("Host Ingress TC[%d], queue %d added at index %d\n", msg_tc, q_id, active_q_id);

	memcpy(&(outtc->rem_inqs_params[active_q_id]), &(giu_gpio_q), sizeof(union giu_gpio_q_params));

	/* Set prod/cons address in response.
	 * we use the qid as the prod/cons idx in the notification space
	 * since that is the how CP-125 HW works
	 */
	resp->resp_data.q_add_resp.q_prod_cons_phys_addr = q_id;

	/* Clear queue structure */
	memset(&giu_gpio_q, 0, sizeof(union giu_gpio_q_params));

	/* Allocate queue from MQA */
	ret = mqa_queue_alloc(nic_pf->mqa, &bpool_q_id);
	if (ret < 0) {
		pr_err("Failed to allocate queue from MQA\n");
		goto ingress_queue_exit;
	}

	/* Init queue parameters */
	giu_gpio_q.rem_q.q_id	      = bpool_q_id;
	giu_gpio_q.rem_q.len	      = params->pf_ingress_data_q_add.q_len;
	giu_gpio_q.rem_q.size	      = params->pf_ingress_data_q_add.q_buf_size;
	giu_gpio_q.rem_q.q_base_pa    = (phys_addr_t)params->pf_ingress_data_q_add.bpool_q_phys_addr;
	giu_gpio_q.rem_q.cons_base_pa = (phys_addr_t)(params->pf_ingress_data_q_add.bpool_q_cons_phys_addr +
									nic_pf->map.host_map.phys_addr);
	giu_gpio_q.rem_q.cons_base_va = (void *)(params->pf_ingress_data_q_add.bpool_q_cons_phys_addr +
									nic_pf->map.host_map.virt_addr);
	giu_gpio_q.rem_q.host_remap   = nic_pf->map.host_map.phys_addr;

	memcpy(&(outtc->rem_poolqs_params[active_q_id]), &(giu_gpio_q), sizeof(union giu_gpio_q_params));

	/* Set queue Id in response message in case of success */
	resp->resp_data.q_add_resp.bpool_q_prod_cons_phys_addr = bpool_q_id;

ingress_queue_exit:

	nic_pf_gen_resp_msg(ret, cmd, resp);

	if (ret < 0) {
		if (q_id > 0) {
			ret = mqa_queue_free(nic_pf->mqa, q_id);
			if (ret)
				pr_err("Failed to free queue Idx %x in MQA\n", q_id);
		}
		if (bpool_q_id > 0) {
			ret = mqa_queue_free(nic_pf->mqa, bpool_q_id);
			if (ret)
				pr_err("Failed to free queue Idx %x in MQA\n", bpool_q_id);
		}
		pr_err("Host ingress TC[%d] Add queue failed\n", msg_tc);
	}

	return ret;
}


/*
 *	nic_pf_egress_queue_add_command
 */
static int nic_pf_egress_queue_add_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	int ret = 0;
	s32 active_q_id;
	u32 msg_tc;
	u32 q_id;

	union giu_gpio_q_params giu_gpio_q;
	struct mgmt_cmd_params *params = &(cmd->params);
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);
	struct giu_gpio_intc_params *intc;

	msg_tc = params->pf_egress_q_add.tc_prio;
	intc = &(q_top->intcs_params.intc_params[msg_tc]);

	pr_debug("Host Egress TC[%d], queue Add (num of queues %d)\n", msg_tc, intc->num_rem_outqs);

	/* Clear queue structure */
	memset(&giu_gpio_q, 0, sizeof(union giu_gpio_q_params));

	/* Allocate queue from MQA */
	ret = mqa_queue_alloc(nic_pf->mqa, &q_id);
	if (ret < 0) {
		pr_err("Failed to allocate queue from MQA\n");
		return ret;
	}

	giu_gpio_q.rem_q.q_id         = q_id;
	giu_gpio_q.rem_q.len          = params->pf_egress_q_add.q_len;
	giu_gpio_q.rem_q.size         = gie_get_desc_size(TX_DESC);
	giu_gpio_q.rem_q.q_base_pa    = (phys_addr_t)params->pf_egress_q_add.q_phys_addr;
	giu_gpio_q.rem_q.cons_base_pa = (phys_addr_t)(params->pf_egress_q_add.q_cons_phys_addr +
										nic_pf->map.host_map.phys_addr);
	giu_gpio_q.rem_q.cons_base_va = (void *)(params->pf_egress_q_add.q_cons_phys_addr +
										nic_pf->map.host_map.virt_addr);
	giu_gpio_q.rem_q.host_remap   = nic_pf->map.host_map.phys_addr;

	active_q_id = tc_q_next_entry_get(intc->rem_outqs_params, intc->num_rem_outqs);
	if (active_q_id < 0) {
		pr_err("Failed to configure queue in Host Egress TC[%d] queue list\n", msg_tc);
		ret = active_q_id;
		goto egress_queue_exit;
	}

	pr_debug("Host Egress TC[%d], queue %d added and index %d\n", msg_tc, q_id, active_q_id);

	memcpy(&(intc->rem_outqs_params[active_q_id]), &(giu_gpio_q), sizeof(union giu_gpio_q_params));

	/* Set queue Id in and prod/cons address in response.
	 * we use the qid as the prod/cons idx in the notification space
	 * since that is the how CP-125 HW works
	 */
	resp->resp_data.q_add_resp.q_prod_cons_phys_addr = q_id;

egress_queue_exit:

	nic_pf_gen_resp_msg(ret, cmd, resp);

	if (ret < 0) {
		if (q_id > 0) {
			mqa_queue_free(nic_pf->mqa, q_id);
			if (ret)
				pr_err("Failed to free queue Idx %x in MQA\n", q_id);
		}
		pr_err("Host Egress TC[%d] Add queue failed\n", msg_tc);
	}

	return ret;
}

static void nic_pf_pp2_set_reserved_bpools(u32 reserved_bpool_map)
{
	int i;

	for (i = 0; i < NMP_PP2_MAX_PKT_PROC; i++)
		used_bpools[i] = reserved_bpool_map;
}

static int nic_pf_pp2_find_free_bpool(u32 pp_id)
{
	int i;

	for (i = 0; i < PP2_BPOOL_NUM_POOLS; i++) {
		if (!((1 << i) & used_bpools[pp_id])) {
			used_bpools[pp_id] |= (1 << i);
			break;
		}
	}
	if (i == PP2_BPOOL_NUM_POOLS) {
		pr_err("no free BPool found!\n");
		return -ENOSPC;
	}
	return i;
}


static int nic_pf_pp2_init_bpools(struct nic_pf *nic_pf)
{
	int				 i;
	struct pp2_bpool_params		 bpool_params;
	int				 err, pool_id;
	char				 name[15];
	u32				 pcount = 0;
	struct nmp_pp2_port_desc	*pdesc;
	u32				 num_pools = 0;

	if (!nic_pf->pp2.reserved_bpools)
		/* No pp2 initialized, just return */
		return 0;

	nic_pf_pp2_set_reserved_bpools(nic_pf->pp2.reserved_bpools);

	pdesc = (struct nmp_pp2_port_desc *)&nic_pf->pp2.ports_desc[pcount];

	for (i = 0; i < pdesc->num_pools; i++) {
		pool_id = nic_pf_pp2_find_free_bpool(pdesc->pp_id);
		if (pool_id < 0) {
			pr_err("free bpool not found!\n");
			return pool_id;
		}
		memset(name, 0, sizeof(name));
		snprintf(name, sizeof(name), "pool-%d:%d", pdesc->pp_id, pool_id);
		memset(&bpool_params, 0, sizeof(bpool_params));
		bpool_params.match = name;
		bpool_params.buff_len = pdesc->pools_desc[i].buff_size;
		pr_info("%s: buff_size %d, num_buffs %d\n", name,
			bpool_params.buff_len, pdesc->pools_desc[i].num_buffs);
		err = pp2_bpool_init(&bpool_params, &pdesc->pools_desc[i].pool);
		if (err)
			goto init_bpools_err;

		if (!pdesc->pools_desc[i].pool) {
			err = -EINVAL;
			pr_err("BPool id%d init failed!\n", pool_id);
			goto init_bpools_err;
		}
		num_pools++;
	}
	return 0;

init_bpools_err:
	for (i = 0; i < num_pools; i++)
		pp2_bpool_deinit(pdesc->pools_desc[i].pool);
	return err;
}

static int nic_pf_pp2_init_ppio(struct nic_pf *nic_pf)
{
	struct pp2_ppio_params		 port_params;
	struct pp2_ppio_inq_params	 inq_params[PP2_HW_PORT_NUM_RXQS];
	char				 name[NMP_PPIO_NAME_MAX];
	int				 i, j, err = 0;
	u32				 pcount = 0;
	struct nmp_pp2_port_desc	*pdesc;
	int				 num_pools;
	struct nmp_pp2_bpool_desc	*pools;

	if (!nic_pf->pp2.ports_desc)
		/* no pp2, just return */
		return 0;

	pdesc = (struct nmp_pp2_port_desc *)&nic_pf->pp2.ports_desc[pcount];
	num_pools = pdesc->num_pools;
	pools = pdesc->pools_desc;

	if (!pp2_ppio_available(pdesc->pp_id, pdesc->ppio_id))
		return -EINVAL;

	memset(&port_params, 0, sizeof(struct pp2_ppio_params));

	memset(name, 0, sizeof(name));
	snprintf(name, sizeof(name), "ppio-%d:%d", pdesc->pp_id, pdesc->ppio_id);
	pr_debug("found port: %s\n", name);
	port_params.match = name;
	port_params.type = PP2_PPIO_T_NIC;
	port_params.eth_start_hdr = PP2_PPIO_HDR_ETH;
	if (pdesc->num_tcs > pdesc->max_num_tcs) {
		pr_err("Number of TC's configured (%d) exceeds PP2 available TC's (%d)\n",
		       pdesc->num_tcs, pdesc->max_num_tcs);
		return -EINVAL;
	}
	port_params.inqs_params.num_tcs = pdesc->num_tcs;
	port_params.inqs_params.hash_type = pdesc->hash_type;
	port_params.specific_type_params.log_port_params.first_inq = pdesc->first_inq;

	for (i = 0; i < pdesc->num_tcs; i++) {
		port_params.inqs_params.tcs_params[i].pkt_offset = pdesc->pkt_offst;
		port_params.inqs_params.tcs_params[i].num_in_qs = pdesc->num_inqs[i];
		for (j = 0; j < pdesc->num_inqs[i]; j++) {
			inq_params[j].size = pdesc->inq_size;
			inq_params[j].mem = NULL;
			inq_params[j].tc_pools_mem_id_index = 0;
		}
		port_params.inqs_params.tcs_params[i].inqs_params = inq_params;
		for (j = 0; j < num_pools; j++)
			port_params.inqs_params.tcs_params[i].pools[0][j] = pools[j].pool;

	}
	port_params.outqs_params.num_outqs = pdesc->num_outqs;
	for (i = 0; i < pdesc->num_outqs; i++)
		port_params.outqs_params.outqs_params[i].size = pdesc->outq_size;

	err = pp2_ppio_init(&port_params, &pdesc->ppio);
	if (err) {
		pr_err("PP-IO init failed (error: %d)!\n", err);
		return err;
	}

	if (!pdesc->ppio) {
		pr_err("PP-IO init failed!\n");
		return -EIO;
	}

	err = pp2_ppio_enable(pdesc->ppio);

	return 0;
}

/*
 *	nic_pf_pf_init_done_command
 */
static void nic_pf_pf_init_done_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	int ret;

#ifdef GIE_NO_MULTI_Q_SUPPORT_FOR_RSS
	struct giu_gpio_init_params *q_top = &(nic_pf->topology_data);

	/* Override Local Ingress number of queues */
	nic_pf->profile_data.lcl_ingress_q_num =
		q_top->outtcs_params.outtc_params[0].num_rem_inqs;

	/* Override Local Egress number of queues */
	nic_pf->profile_data.lcl_egress_q_num =
		q_top->intcs_params.intc_params[0].num_rem_outqs;

	/* Initialize local queues database */
	ret = nic_pf_topology_local_queue_init(nic_pf);
	if (ret)
		pr_err("Failed to update local DB queue info\n");

	/* Allocate and configure local queues in the database */
	ret = nic_pf_topology_local_queue_cfg(nic_pf);
	if (ret)
		pr_err("Failed to configure PF regfile\n");
#endif

	ret = giu_bpool_init(&(nic_pf->topology_data), &(nic_pf->giu_bpool));
	if (ret)
		pr_err("Failed to init giu bpool\n");

	ret = giu_gpio_init(&(nic_pf->topology_data), &(nic_pf->giu_gpio));
	if (ret)
		pr_err("Failed to init giu gpio\n");



	ret = nic_pf_pp2_init_bpools(nic_pf);
	if (ret)
		pr_err("nic_pf__pp2_init_bpools failed\n");

	ret = nic_pf_pp2_init_ppio(nic_pf);
	if (ret)
		pr_err("nic_pf_pp2_init_ppio failed\n");

	/* Indicate nmp init_done ready */
	nic_pf->f_ready_cb(nic_pf);
	ret = nic_pf_config_topology_and_update_regfile(nic_pf);
	if (ret)
		pr_err("Failed to configure PF regfile\n");
	/* Generate response message */
	nic_pf_gen_resp_msg(ret, cmd, resp);
}

/*
 *	nic_pf_mgmt_echo_command
 */
static int nic_pf_mgmt_echo_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	pr_debug("Management echo message idx:%d.\n", cmd->cmd_idx);

	nic_pf = nic_pf;

	/* Generate response message */
	nic_pf_gen_resp_msg(0, cmd, resp);

	return 0;
}


/*
 *	nic_pf_link_status_command
 */
static int nic_pf_link_status_command(struct nic_pf *nic_pf,
					struct cmd_desc *cmd, struct notif_desc *resp)
{
	pr_debug("Link status message idx:%d.\n", cmd->cmd_idx);

	nic_pf = nic_pf;

	/* Generate response message */
	nic_pf_gen_resp_msg(0, cmd, resp);

	/* TODO: check PP2 link and report it back to the host */
	pr_warn("GIU Link status is set to 'up'\n");
	resp->resp_data.link_status = 1;

	return 0;
}


/*
 *	nic_pf_close_command
 */
static int nic_pf_close_command(struct nic_pf *nic_pf,
				struct cmd_desc *cmd, struct notif_desc *resp)
{
	int ret;

	pr_debug("Close message idx:%d.\n", cmd->cmd_idx);
	pr_info("Closing PF data path resources\n");

	/* Close stages:
	 * 1) NMP should disable the PP2 and GIU disable
	 * 2) Inform the guest app about if down (it should remove GIU and PP2)
	 * 3) Wait till guest app completes the operation (serialized file is deleted)
	 * 4) De-init PP2 and GIU
	 * 5) Free resources
	 *
	 * Note: only stage 5 implemented below.
	 * TODO: implement other stages
	 */

	/* Free Data Qs and Un-register in MQA/GIE */
	pr_debug("Free Data Qs\n");
	giu_gpio_deinit(nic_pf->giu_gpio);

	/* Free BPools and Un-register in MQA/GIE */
	pr_debug("Free BM Qs\n");
	giu_bpool_deinit(nic_pf->giu_bpool);

	/*Free DB TCs */
	pr_debug("Free DB structures\n");
	ret = nic_pf_topology_tc_free(nic_pf);
	if (ret)
		pr_err("Failed to free DB resources\n");

	/* Generate response message */
	nic_pf_gen_resp_msg(0, cmd, resp);

	return 0;
}


/*
 *	nic_pf_process_command
 *
 *	This function process all PF initialization commands
 *
 *	@param[in]	nic_pf - pointer to NIC PF object
 *	@param[in]	cmd - pointer to cmd_desc object
 *	@param[out]	resp - pointer to notif_desc object
 *
 *	@retval	0 on success
 *	@retval	error-code otherwise
 */
int nic_pf_process_command(void *nic_pf, u8 cmd_code, void *cmd)
{
	int ret;
	struct notif_desc resp;

	switch (cmd_code) {

	case CC_PF_ENABLE:
		break;

	case CC_PF_DISABLE:
		break;

	case CC_PF_INIT:
		ret = nic_pf_pf_init_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_INIT message failed\n");
		break;

	case CC_PF_EGRESS_TC_ADD:
		ret = nic_pf_egress_tc_add_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_EGRESS_TC_ADD message failed\n");
		break;

	case CC_PF_EGRESS_DATA_Q_ADD:
		ret = nic_pf_egress_queue_add_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_EGRESS_DATA_Q_ADD message failed\n");
		break;

	case CC_PF_INGRESS_TC_ADD:
		ret = nic_pf_ingress_tc_add_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_INGRESS_TC_ADD message failed\n");
		break;

	case CC_PF_INGRESS_DATA_Q_ADD:
		ret = nic_pf_ingress_queue_add_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_INGRESS_DATA_Q_ADD message failed\n");
		break;

	case CC_PF_INIT_DONE:
		nic_pf_pf_init_done_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		break;

	case CC_PF_MGMT_ECHO:
		ret = nic_pf_mgmt_echo_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_MGMT_ECHO message failed\n");
		break;

	case CC_PF_LINK_STATUS:
		ret = nic_pf_link_status_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_LINK_STATUS message failed\n");
		break;

	case CC_PF_CLOSE:
		ret = nic_pf_close_command((struct nic_pf *)nic_pf, (struct cmd_desc *)cmd, &resp);
		if (ret)
			pr_err("PF_IF_DOWN message failed\n");
		break;

	default:
		/* Unknown command code */
		pr_err("Unknown command code %d!! Unable to process command.\n", cmd_code);
		resp.status = NOTIF_STATUS_FAIL;

		break;
	}

	ret = nmdisp_send(((struct nic_pf *)nic_pf)->nmdisp, CDT_PF,
					  ((struct nic_pf *)nic_pf)->pf_id, 0, (void *)&resp);
	if (ret) {
		pr_err("failed to send response message\n");
		return ret;
	}

	return 0;
}

