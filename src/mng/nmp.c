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

#include "std_internal.h"
#include "mng/mv_nmp.h"
#include "db.h"
#include "dev_mng.h"
#include "mng/dispatch.h"
#include "config.h"

int nmp_init(struct nmp_params *params, struct nmp **nmp)
{
	int ret;

	pr_info("Starting %s %s\n", "giu_main", VERSION);

	*nmp = kcalloc(1, sizeof(struct nmp), GFP_KERNEL);
	if (*nmp == NULL) {
		pr_err("Failed to allocate NMP handler\n");
		return -ENOMEM;
	}

	(*nmp)->nic_pf.profile_data.lcl_egress_q_num   = params->lfs_params->pf.lcl_egress_q_num;
	(*nmp)->nic_pf.profile_data.lcl_egress_q_size  = params->lfs_params->pf.lcl_egress_q_size;
	(*nmp)->nic_pf.profile_data.lcl_ingress_q_num  = params->lfs_params->pf.lcl_ingress_q_num;
	(*nmp)->nic_pf.profile_data.lcl_ingress_q_size = params->lfs_params->pf.lcl_ingress_q_size;
	(*nmp)->nic_pf.profile_data.lcl_bm_q_num       = params->lfs_params->pf.lcl_bm_q_num;
	(*nmp)->nic_pf.profile_data.lcl_bm_q_size      = params->lfs_params->pf.lcl_bm_q_size;
	(*nmp)->nic_pf.profile_data.lcl_bm_buf_size    = params->lfs_params->pf.lcl_bm_buf_size;

	ret = dev_mng_init(*nmp);
	if (ret) {
		pr_err("Management init failed with error %d\n", ret);
		kfree(nmp);
		exit(ret);
	}

	pr_info("Completed management init\n");

	return 0;
}

int nmp_schedule(struct nmp *nmp, enum nmp_sched_type type)
{
	switch (type) {

	case NMP_SCHED_MNG:
		gie_schedule(nmp->nic_pf.gie.mng_gie, 0, 1);
		nmdisp_dispatch(nmp->nmdisp);
		break;

	case NMP_SCHED_RX:
		gie_schedule(nmp->nic_pf.gie.rx_gie, 0, 1);
		break;

	case NMP_SCHED_TX:
		gie_schedule(nmp->nic_pf.gie.tx_gie, 0, 1);
		break;
	}
	return 0;
}

