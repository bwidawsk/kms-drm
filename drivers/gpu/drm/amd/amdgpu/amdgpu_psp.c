/*
 * Copyright 2016 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Huang Rui
 *
 */

#include <linux/firmware.h>
#include "drmP.h"
#include "amdgpu.h"
#include "amdgpu_psp.h"
#include "amdgpu_ucode.h"
#include "soc15_common.h"
#include "psp_v3_1.h"

static void psp_set_funcs(struct amdgpu_device *adev);

static int psp_early_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;

	psp_set_funcs(adev);

	return 0;
}

static int psp_sw_init(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct psp_context *psp = &adev->psp;
	int ret;

	switch (adev->asic_type) {
	case CHIP_VEGA10:
		psp->init_microcode = psp_v3_1_init_microcode;
		psp->bootloader_load_sysdrv = psp_v3_1_bootloader_load_sysdrv;
		psp->bootloader_load_sos = psp_v3_1_bootloader_load_sos;
		psp->prep_cmd_buf = psp_v3_1_prep_cmd_buf;
		psp->ring_init = psp_v3_1_ring_init;
		psp->ring_create = psp_v3_1_ring_create;
		psp->cmd_submit = psp_v3_1_cmd_submit;
		psp->compare_sram_data = psp_v3_1_compare_sram_data;
		psp->smu_reload_quirk = psp_v3_1_smu_reload_quirk;
		break;
	default:
		return -EINVAL;
	}

	psp->adev = adev;

	ret = psp_init_microcode(psp);
	if (ret) {
		DRM_ERROR("Failed to load psp firmware!\n");
		return ret;
	}

	return 0;
}

static int psp_sw_fini(void *handle)
{
	return 0;
}

int psp_wait_for(struct psp_context *psp, uint32_t reg_index,
		 uint32_t reg_val, uint32_t mask, bool check_changed)
{
	uint32_t val;
	int i;
	struct amdgpu_device *adev = psp->adev;

	val = RREG32(reg_index);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (check_changed) {
			if (val != reg_val)
				return 0;
		} else {
			if ((val & mask) == reg_val)
				return 0;
		}
		udelay(1);
	}

	return -ETIME;
}

static int
psp_cmd_submit_buf(struct psp_context *psp,
		   struct amdgpu_firmware_info *ucode,
		   struct psp_gfx_cmd_resp *cmd, uint64_t fence_mc_addr,
		   int index)
{
	int ret;
	struct amdgpu_bo *cmd_buf_bo;
	uint64_t cmd_buf_mc_addr;
	struct psp_gfx_cmd_resp *cmd_buf_mem;
	struct amdgpu_device *adev = psp->adev;

	ret = amdgpu_bo_create_kernel(adev, PSP_CMD_BUFFER_SIZE, PAGE_SIZE,
				      AMDGPU_GEM_DOMAIN_VRAM,
				      &cmd_buf_bo, &cmd_buf_mc_addr,
				      (void **)&cmd_buf_mem);
	if (ret)
		return ret;

	memset(cmd_buf_mem, 0, PSP_CMD_BUFFER_SIZE);

	memcpy(cmd_buf_mem, cmd, sizeof(struct psp_gfx_cmd_resp));

	ret = psp_cmd_submit(psp, ucode, cmd_buf_mc_addr,
			     fence_mc_addr, index);

	while (*((unsigned int *)psp->fence_buf) != index) {
		msleep(1);
	}

	amdgpu_bo_free_kernel(&cmd_buf_bo,
			      &cmd_buf_mc_addr,
			      (void **)&cmd_buf_mem);

	return ret;
}

static void psp_prep_tmr_cmd_buf(struct psp_gfx_cmd_resp *cmd,
				 uint64_t tmr_mc, uint32_t size)
{
	cmd->cmd_id = GFX_CMD_ID_SETUP_TMR;
	cmd->cmd.cmd_setup_tmr.buf_phy_addr_lo = (uint32_t)tmr_mc;
	cmd->cmd.cmd_setup_tmr.buf_phy_addr_hi = (uint32_t)(tmr_mc >> 32);
	cmd->cmd.cmd_setup_tmr.buf_size = size;
}

/* Set up Trusted Memory Region */
static int psp_tmr_init(struct psp_context *psp)
{
	int ret;

	/*
	 * Allocate 3M memory aligned to 1M from Frame Buffer (local
	 * physical).
	 *
	 * Note: this memory need be reserved till the driver
	 * uninitializes.
	 */
	ret = amdgpu_bo_create_kernel(psp->adev, 0x300000, 0x100000,
				      AMDGPU_GEM_DOMAIN_VRAM,
				      &psp->tmr_bo, &psp->tmr_mc_addr, &psp->tmr_buf);

	return ret;
}

static int psp_tmr_load(struct psp_context *psp)
{
	int ret;
	struct psp_gfx_cmd_resp *cmd;

	cmd = kzalloc(sizeof(struct psp_gfx_cmd_resp), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	psp_prep_tmr_cmd_buf(cmd, psp->tmr_mc_addr, 0x300000);

	ret = psp_cmd_submit_buf(psp, NULL, cmd,
				 psp->fence_buf_mc_addr, 1);
	if (ret)
		goto failed;

	kfree(cmd);

	return 0;

failed:
	kfree(cmd);
	return ret;
}

static void psp_prep_asd_cmd_buf(struct psp_gfx_cmd_resp *cmd,
				 uint64_t asd_mc, uint64_t asd_mc_shared,
				 uint32_t size, uint32_t shared_size)
{
	cmd->cmd_id = GFX_CMD_ID_LOAD_ASD;
	cmd->cmd.cmd_load_ta.app_phy_addr_lo = lower_32_bits(asd_mc);
	cmd->cmd.cmd_load_ta.app_phy_addr_hi = upper_32_bits(asd_mc);
	cmd->cmd.cmd_load_ta.app_len = size;

	cmd->cmd.cmd_load_ta.cmd_buf_phy_addr_lo = lower_32_bits(asd_mc_shared);
	cmd->cmd.cmd_load_ta.cmd_buf_phy_addr_hi = upper_32_bits(asd_mc_shared);
	cmd->cmd.cmd_load_ta.cmd_buf_len = shared_size;
}

static int psp_asd_init(struct psp_context *psp)
{
	int ret;

	/*
	 * Allocate 16k memory aligned to 4k from Frame Buffer (local
	 * physical) for shared ASD <-> Driver
	 */
	ret = amdgpu_bo_create_kernel(psp->adev, PSP_ASD_SHARED_MEM_SIZE,
				      PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM,
				      &psp->asd_shared_bo,
				      &psp->asd_shared_mc_addr,
				      &psp->asd_shared_buf);

	return ret;
}

static int psp_asd_load(struct psp_context *psp)
{
	int ret;
	struct psp_gfx_cmd_resp *cmd;

	cmd = kzalloc(sizeof(struct psp_gfx_cmd_resp), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	memset(psp->fw_pri_buf, 0, PSP_1_MEG);
	memcpy(psp->fw_pri_buf, psp->asd_start_addr, psp->asd_ucode_size);

	psp_prep_asd_cmd_buf(cmd, psp->fw_pri_mc_addr, psp->asd_shared_mc_addr,
			     psp->asd_ucode_size, PSP_ASD_SHARED_MEM_SIZE);

	ret = psp_cmd_submit_buf(psp, NULL, cmd,
				 psp->fence_buf_mc_addr, 2);

	kfree(cmd);

	return ret;
}

static int psp_hw_start(struct psp_context *psp)
{
	int ret;

	ret = psp_bootloader_load_sysdrv(psp);
	if (ret)
		return ret;

	ret = psp_bootloader_load_sos(psp);
	if (ret)
		return ret;

	ret = psp_ring_create(psp, PSP_RING_TYPE__KM);
	if (ret)
		return ret;

	ret = psp_tmr_load(psp);
	if (ret)
		return ret;

	ret = psp_asd_load(psp);
	if (ret)
		return ret;

	return 0;
}

static int psp_np_fw_load(struct psp_context *psp)
{
	int i, ret;
	struct amdgpu_firmware_info *ucode;
	struct amdgpu_device* adev = psp->adev;

	for (i = 0; i < adev->firmware.max_ucodes; i++) {
		ucode = &adev->firmware.ucode[i];
		if (!ucode->fw)
			continue;

		if (ucode->ucode_id == AMDGPU_UCODE_ID_SMC &&
		    psp_smu_reload_quirk(psp))
			continue;

		ret = psp_prep_cmd_buf(ucode, psp->cmd);
		if (ret)
			return ret;

		ret = psp_cmd_submit_buf(psp, ucode, psp->cmd,
					 psp->fence_buf_mc_addr, i + 3);
		if (ret)
			return ret;

#if 0
		/* check if firmware loaded sucessfully */
		if (!amdgpu_psp_check_fw_loading_status(adev, i))
			return -EINVAL;
#endif
	}

	return 0;
}

static int psp_load_fw(struct amdgpu_device *adev)
{
	int ret;
	struct psp_context *psp = &adev->psp;
	struct psp_gfx_cmd_resp *cmd;

	cmd = kzalloc(sizeof(struct psp_gfx_cmd_resp), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	psp->cmd = cmd;

	ret = amdgpu_bo_create_kernel(adev, PSP_1_MEG, PSP_1_MEG,
				      AMDGPU_GEM_DOMAIN_GTT,
				      &psp->fw_pri_bo,
				      &psp->fw_pri_mc_addr,
				      &psp->fw_pri_buf);
	if (ret)
		goto failed;

	ret = amdgpu_bo_create_kernel(adev, PSP_FENCE_BUFFER_SIZE, PAGE_SIZE,
				      AMDGPU_GEM_DOMAIN_VRAM,
				      &psp->fence_buf_bo,
				      &psp->fence_buf_mc_addr,
				      &psp->fence_buf);
	if (ret)
		goto failed_mem1;

	memset(psp->fence_buf, 0, PSP_FENCE_BUFFER_SIZE);

	ret = psp_ring_init(psp, PSP_RING_TYPE__KM);
	if (ret)
		goto failed_mem1;

	ret = psp_tmr_init(psp);
	if (ret)
		goto failed_mem;

	ret = psp_asd_init(psp);
	if (ret)
		goto failed_mem;

	ret = psp_hw_start(psp);
	if (ret)
		goto failed_mem;

	ret = psp_np_fw_load(psp);
	if (ret)
		goto failed_mem;

	kfree(cmd);

	return 0;

failed_mem:
	amdgpu_bo_free_kernel(&psp->fence_buf_bo,
			      &psp->fence_buf_mc_addr, &psp->fence_buf);
failed_mem1:
	amdgpu_bo_free_kernel(&psp->fw_pri_bo,
			      &psp->fw_pri_mc_addr, &psp->fw_pri_buf);
failed:
	kfree(cmd);
	return ret;
}

static int psp_hw_init(void *handle)
{
	int ret;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;


	if (adev->firmware.load_type != AMDGPU_FW_LOAD_PSP)
		return 0;

	mutex_lock(&adev->firmware.mutex);
	/*
	 * This sequence is just used on hw_init only once, no need on
	 * resume.
	 */
	ret = amdgpu_ucode_init_bo(adev);
	if (ret)
		goto failed;

	ret = psp_load_fw(adev);
	if (ret) {
		DRM_ERROR("PSP firmware loading failed\n");
		goto failed;
	}

	mutex_unlock(&adev->firmware.mutex);
	return 0;

failed:
	adev->firmware.load_type = AMDGPU_FW_LOAD_DIRECT;
	mutex_unlock(&adev->firmware.mutex);
	return -EINVAL;
}

static int psp_hw_fini(void *handle)
{
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct psp_context *psp = &adev->psp;

	if (adev->firmware.load_type == AMDGPU_FW_LOAD_PSP)
		amdgpu_ucode_fini_bo(adev);

	if (psp->tmr_buf)
		amdgpu_bo_free_kernel(&psp->tmr_bo, &psp->tmr_mc_addr, &psp->tmr_buf);

	if (psp->fw_pri_buf)
		amdgpu_bo_free_kernel(&psp->fw_pri_bo,
				      &psp->fw_pri_mc_addr, &psp->fw_pri_buf);

	if (psp->fence_buf_bo)
		amdgpu_bo_free_kernel(&psp->fence_buf_bo,
				      &psp->fence_buf_mc_addr, &psp->fence_buf);

	return 0;
}

static int psp_suspend(void *handle)
{
	return 0;
}

static int psp_resume(void *handle)
{
	int ret;
	struct amdgpu_device *adev = (struct amdgpu_device *)handle;
	struct psp_context *psp = &adev->psp;

	if (adev->firmware.load_type != AMDGPU_FW_LOAD_PSP)
		return 0;

	DRM_INFO("PSP is resuming...\n");

	mutex_lock(&adev->firmware.mutex);

	ret = psp_hw_start(psp);
	if (ret)
		goto failed;

	ret = psp_np_fw_load(psp);
	if (ret)
		goto failed;

	mutex_unlock(&adev->firmware.mutex);

	return 0;

failed:
	DRM_ERROR("PSP resume failed\n");
	mutex_unlock(&adev->firmware.mutex);
	return ret;
}

static bool psp_check_fw_loading_status(struct amdgpu_device *adev,
					enum AMDGPU_UCODE_ID ucode_type)
{
	struct amdgpu_firmware_info *ucode = NULL;

	if (adev->firmware.load_type != AMDGPU_FW_LOAD_PSP) {
		DRM_INFO("firmware is not loaded by PSP\n");
		return true;
	}

	if (!adev->firmware.fw_size)
		return false;

	ucode = &adev->firmware.ucode[ucode_type];
	if (!ucode->fw || !ucode->ucode_size)
		return false;

	return psp_compare_sram_data(&adev->psp, ucode, ucode_type);
}

static int psp_set_clockgating_state(void *handle,
				     enum amd_clockgating_state state)
{
	return 0;
}

static int psp_set_powergating_state(void *handle,
				     enum amd_powergating_state state)
{
	return 0;
}

const struct amd_ip_funcs psp_ip_funcs = {
	.name = "psp",
	.early_init = psp_early_init,
	.late_init = NULL,
	.sw_init = psp_sw_init,
	.sw_fini = psp_sw_fini,
	.hw_init = psp_hw_init,
	.hw_fini = psp_hw_fini,
	.suspend = psp_suspend,
	.resume = psp_resume,
	.is_idle = NULL,
	.wait_for_idle = NULL,
	.soft_reset = NULL,
	.set_clockgating_state = psp_set_clockgating_state,
	.set_powergating_state = psp_set_powergating_state,
};

static const struct amdgpu_psp_funcs psp_funcs = {
	.check_fw_loading_status = psp_check_fw_loading_status,
};

static void psp_set_funcs(struct amdgpu_device *adev)
{
	if (NULL == adev->firmware.funcs)
		adev->firmware.funcs = &psp_funcs;
}

const struct amdgpu_ip_block_version psp_v3_1_ip_block =
{
	.type = AMD_IP_BLOCK_TYPE_PSP,
	.major = 3,
	.minor = 1,
	.rev = 0,
	.funcs = &psp_ip_funcs,
};
