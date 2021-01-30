#include <sys/types.h>
#include <alsa/asoundlib.h>
#include <opl3.h>
#include <math.h>

#include "x86_opl3sw.h"
#include "x86.h"

#define PCM_DEVICE   "default"
#define PCM_CHANNELS 2
#define PCM_FRAMES   32

#define CHECK_ERROR(f) if ((rc = (f)) < 0) { printf("OPL3_SW Alsa ERROR %s\n", snd_strerror(rc)); }

snd_pcm_t *pcm_handle;
snd_pcm_hw_params_t *hw_params;
snd_pcm_sw_params_t *sw_params;
snd_async_handler_t *pcm_callback;
uint32_t samplerate = 44100;
int16_t * samplebuf;
snd_pcm_uframes_t periodsize = PCM_FRAMES * PCM_CHANNELS * sizeof(int16_t);

opl3_chip chip;
uint16_t chip_address = 0;
static uint16_t write_reg_buf[256];
static bool init = false;

int x86_opl3sw_init();
void x86_opl3sw_cleanup();
void x86_opl3sw_opl_writereg(uint8_t a, uint8_t data);
void x86_opl3sw_sample(snd_async_handler_t *pcm_callback);

void x86_opl3sw_poll(uint32_t baseaddr) {
	uint32_t status = x86_dma_get(baseaddr);
	
	if (status) {
		uint16_t num_reg_writes = (uint16_t)status;
		
		if (!init) {
			if (x86_opl3sw_init() == 0) {
				x86_opl3sw_reset();
				init = true;
			}
		}
		
		if (num_reg_writes > sizeof(write_reg_buf) >> 1) num_reg_writes = sizeof(write_reg_buf) >> 1;
		if (num_reg_writes == 1) {
			write_reg_buf[0] = (uint16_t)x86_dma_get(baseaddr + 1);
		}
		else {
			// x86_dma_recvbuf can only handle multiples of uint32_t
			if ((num_reg_writes % 2) != 0) --num_reg_writes;
			x86_dma_recvbuf(baseaddr + 1, num_reg_writes >> 1, (uint32_t*)write_reg_buf);
		}
		
		for (uint16_t i=0; i<num_reg_writes; i++) {
			uint8_t a = (write_reg_buf[i] >> 8) & 0xFF;
			uint8_t data = write_reg_buf[i] & 0xFF;
			x86_opl3sw_opl_writereg(a, data);
			//printf("x86_opl3sw_opl_writereg(%02X,%02X)\n", a, data);
		}
	}
	
	//x86_opl3sw_sample();
}

void x86_opl3sw_reset() {    
	OPL3_Reset(&chip, samplerate);
}

int x86_opl3sw_init() {
	int rc;
	
	// Open the PCM device in playback mode
	CHECK_ERROR(snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0));
	if (rc < 0) return -1;
	
	// Set HW parameters
	snd_pcm_hw_params_alloca(&hw_params);
	CHECK_ERROR(snd_pcm_hw_params_any(pcm_handle, hw_params));
	CHECK_ERROR(snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED));
	CHECK_ERROR(snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE));
	CHECK_ERROR(snd_pcm_hw_params_set_channels(pcm_handle, hw_params, PCM_CHANNELS));
	CHECK_ERROR(snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &samplerate, 0));
	CHECK_ERROR(snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &periodsize, NULL));
	CHECK_ERROR(snd_pcm_hw_params(pcm_handle, hw_params));

	// Set SW parameters
	CHECK_ERROR(snd_pcm_sw_params_malloc(&sw_params));
	CHECK_ERROR(snd_pcm_sw_params_current(pcm_handle, sw_params));
	CHECK_ERROR(snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, periodsize << 1));
	CHECK_ERROR(snd_pcm_sw_params_set_avail_min(pcm_handle, sw_params, periodsize));
	CHECK_ERROR(snd_pcm_sw_params(pcm_handle, sw_params));
	
	samplebuf = (int16_t*)malloc(periodsize);
	memset(samplebuf, 0, periodsize);
	
	CHECK_ERROR(snd_pcm_prepare(pcm_handle));
	CHECK_ERROR(snd_pcm_writei(pcm_handle, samplebuf, periodsize));
	CHECK_ERROR(snd_pcm_writei(pcm_handle, samplebuf, periodsize));
	CHECK_ERROR(snd_async_add_pcm_handler(&pcm_callback, pcm_handle, x86_opl3sw_sample, NULL));
	
	printf("OPL3_SW PCM name: '%s'\n", snd_pcm_name(pcm_handle));
	printf("OPL3_SW PCM state: %s\n", snd_pcm_state_name(snd_pcm_state(pcm_handle)));
	printf("OPL3_SW PCM rate: %d bps\n", samplerate);
	printf("OPL3_SW PCM periodsize: %lu bytes\n", periodsize);
	
	// Start alsa processing
	snd_pcm_start(pcm_handle);

	return 0;
}

void x86_opl3sw_opl_writereg(uint8_t a, uint8_t data) {
	switch (a & 3) {
		case 0: /* address port 0 (register set #1) */
			chip_address = data;
			break;

		case 1: /* data port - ignore A1 */
		case 3: /* data port - ignore A1 */
			OPL3_WriteReg(&chip, chip_address, data);
			break;

		case 2: /* address port 1 (register set #2) */
			if (chip.newm) chip_address = data | 0x100; /* OPL3 mode */
			else {
				if (data == 5) chip_address = data | 0x100; /* in OPL2 mode the only accessible in set #2 is register 0x05 */
				else chip_address = data;  /* verified range: 0x01, 0x04, 0x20-0xef(set #2 becomes set #1 in opl2 mode) */
			}
			break;
		}
}

int sini = 0;

void x86_opl3sw_sample(snd_async_handler_t *pcm_callback) {
	int rc;
	snd_pcm_t *pcm_handle = snd_async_handler_get_pcm(pcm_callback);
	snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_handle);
	
	//printf("x86_opl3sw_sample(%d)\n", (int)avail);
	
	while (avail >= (snd_pcm_sframes_t)periodsize) {
		OPL3_GenerateStream(&chip, samplebuf, PCM_FRAMES);
		CHECK_ERROR(snd_pcm_writei(pcm_handle, samplebuf, PCM_FRAMES));
		if (rc == -EPIPE) {
			printf("OPL3_SW ALSA BUFFER UNDERFLOW.\n");
			snd_pcm_prepare(pcm_handle);
			memset(samplebuf, 0, periodsize);
			CHECK_ERROR(snd_pcm_writei(pcm_handle, samplebuf, PCM_FRAMES));
			CHECK_ERROR(snd_pcm_writei(pcm_handle, samplebuf, PCM_FRAMES));
		}
		avail = snd_pcm_avail_update(pcm_handle);
	}
}

void x86_opl3sw_cleanup() {
	int rc;
	
	CHECK_ERROR(snd_async_del_handler(pcm_callback));
	CHECK_ERROR(snd_pcm_drain(pcm_handle));
	CHECK_ERROR(snd_pcm_close(pcm_handle));
	snd_pcm_hw_params_free(hw_params);
	snd_pcm_sw_params_free(sw_params);
	free(samplebuf);
}
