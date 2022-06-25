/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include <sys/param.h>
#include <string.h>
#include "sdkconfig.h"
#include <rom/cache.h>
#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "misc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_pm.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_rom_gpio.h"
#include "soc/soc_caps.h"
#include "soc/rtc.h" // for querying XTAL clock
#include "hal/dma_types.h"
#include "hal/gpio_hal.h"
#include "esp_private/gdma.h"
#include "driver/gpio.h"
// #include "esp_private/periph_ctrl.h"
#include "driver/periph_ctrl.h"
#if CONFIG_SPIRAM
#include "spiram.h"
#endif
#include "esp_lcd_common.h"
#include "soc/lcd_periph.h"
#include "hal/lcd_hal.h"
#include "hal/lcd_ll.h"
#include <rom/cache.h>

#if CONFIG_LCD_RGB_ISR_IRAM_SAFE
#define LCD_RGB_INTR_ALLOC_FLAGS	 (ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_INTRDISABLED)
#else
#define LCD_RGB_INTR_ALLOC_FLAGS	 ESP_INTR_FLAG_INTRDISABLED
#endif

static const char *TAG = "lcd_panel.rgb";

typedef struct esp_rgb_panel_t esp_rgb_panel_t;


static esp_err_t rgb_panel_del(esp_lcd_panel_t *panel);
static esp_err_t rgb_panel_reset(esp_lcd_panel_t *panel);
static esp_err_t rgb_panel_init(esp_lcd_panel_t *panel);
static esp_err_t rgb_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t rgb_panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t rgb_panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t rgb_panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t rgb_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t rgb_panel_disp_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t lcd_rgb_panel_select_periph_clock(esp_rgb_panel_t *panel, lcd_clock_source_t clk_src);
static esp_err_t lcd_rgb_panel_create_trans_link(esp_rgb_panel_t *panel);
static esp_err_t lcd_rgb_panel_configure_gpio(esp_rgb_panel_t *panel, const esp_lcd_rgb_panel_config_t *panel_config);
static void lcd_rgb_panel_start_transmission(esp_rgb_panel_t *rgb_panel);
static void lcd_default_isr_handler(void *args);


struct esp_rgb_panel_t {
	esp_lcd_panel_t base;  // Base class of generic lcd panel
	int panel_id;		   // LCD panel ID
	lcd_hal_context_t hal; // Hal layer object
	size_t data_width;	   // Number of data lines (e.g. for RGB565, the data width is 16)
	size_t sram_trans_align;  // Alignment for framebuffer that allocated in SRAM
	size_t psram_trans_align; // Alignment for framebuffer that allocated in PSRAM
	int disp_gpio_num;	   // Display control GPIO, which is used to perform action like "disp_off"
	intr_handle_t intr;	   // LCD peripheral interrupt handle
	esp_pm_lock_handle_t pm_lock; // Power management lock
	size_t num_dma_nodes;  // Number of DMA descriptors that used to carry the frame buffer
	uint8_t *fb;		   // Frame buffer.
	size_t fb_size;		   // Size of frame buffer
	int data_gpio_nums[SOC_LCD_RGB_DATA_WIDTH]; // GPIOs used for data lines, we keep these GPIOs for action like "invert_color"
	size_t resolution_hz;	 // Peripheral clock resolution
	esp_lcd_rgb_timing_t timings;	// RGB timing parameters (e.g. pclk, sync pulse, porch width)
	gdma_channel_handle_t dma_chan; // DMA channel handle
	esp_lcd_rgb_panel_frame_trans_done_cb_t on_frame_trans_done; // Callback, invoked after frame trans done
	int bounce_buffer_size_bytes;	//If not-zero, the driver uses a bounce buffer in internal memory to DMA from. It's in bytes here.
	uint8_t *bounce_buffer[2];		//Pointer to the bounce buffers
	int bounce_buf_frame_start;		//If frame restarts, which bb has the initial frame data?
	esp_lcd_rgb_panel_bounce_buf_fill_cb_t on_bounce_empty; // If we use a bounce buffer, this function gets called to fill it rather than copying from the framebuffer
	void *bounce_buffer_cb_user_ctx;   //Callback data pointer
	void *user_ctx;				   // Reserved user's data of callback functions
	int bounce_pos_px;				   // Position in whatever source material is used for the bounce buffer, in pixels
	int x_gap;						// Extra gap in x coordinate, it's used when calculate the flush window
	int y_gap;						// Extra gap in y coordinate, it's used when calculate the flush window
	struct {
		unsigned int disp_en_level: 1; // The level which can turn on the screen by `disp_gpio_num`
		unsigned int stream_mode: 1;   // If set, the LCD transfers data continuously, otherwise, it stops refreshing the LCD when transaction done
		unsigned int fb_in_psram: 1;   // Whether the frame buffer is in PSRAM
	} flags;
	dma_descriptor_t dma_nodes[]; // DMA descriptor pool of size `num_dma_nodes`
};

esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t *rgb_panel_config, esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
	esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
	esp_err_t ret = ESP_OK;
	esp_rgb_panel_t *rgb_panel = NULL;
	ESP_GOTO_ON_FALSE(rgb_panel_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid parameter");
	ESP_GOTO_ON_FALSE(rgb_panel_config->data_width == 16, ESP_ERR_NOT_SUPPORTED, err, TAG,
					  "unsupported data width %d", rgb_panel_config->data_width);
	ESP_GOTO_ON_FALSE(!(rgb_panel_config->bounce_buffer_size_px == 0 && rgb_panel_config->on_bounce_empty != NULL), 
					  ESP_ERR_INVALID_ARG, err, TAG, "cannot have bounce empty callback without having a bounce buffer");

#if CONFIG_LCD_RGB_ISR_IRAM_SAFE
	if (rgb_panel_config->on_frame_trans_done) {
		ESP_RETURN_ON_FALSE(esp_ptr_in_iram(rgb_panel_config->on_frame_trans_done), ESP_ERR_INVALID_ARG, TAG, "on_frame_trans_done callback not in IRAM");
	}
	if (rgb_panel_config->user_ctx) {
		ESP_RETURN_ON_FALSE(esp_ptr_internal(rgb_panel_config->user_ctx), ESP_ERR_INVALID_ARG, TAG, "user context not in internal RAM");
	}
#endif

    size_t fb_size;

	// calculate the number of DMA descriptors
    if (rgb_panel_config->fb == NULL) {
	    fb_size = rgb_panel_config->timings.h_res * rgb_panel_config->timings.v_res * rgb_panel_config->data_width / 8;
    } else {
        fb_size = rgb_panel_config->fb_size;
    }
	size_t num_dma_nodes;
	int bounce_bytes = 0;
	if (rgb_panel_config->bounce_buffer_size_px == 0) {
		// No bounce buffers. DMA descriptors need to fit entire framebuffer
		num_dma_nodes = (fb_size + DMA_DESCRIPTOR_BUFFER_MAX_SIZE - 1) / DMA_DESCRIPTOR_BUFFER_MAX_SIZE;
	} else {
		//The FB needs to be an integer multiple of the size of the two bounce buffers
		//combined (so we end on the end of the second bounce buffer). Adjust the size
		//of the bounce buffers if it is not.
		int no_pixels=rgb_panel_config->timings.h_res * rgb_panel_config->timings.v_res;
		bounce_bytes=rgb_panel_config->bounce_buffer_size_px * 2;
		if (no_pixels % (rgb_panel_config->bounce_buffer_size_px * 2)) {
			//Search for some value that does work. Yes, this is a stupidly simple algo, but it only
			//needs to run on startup.
			for (int a=rgb_panel_config->bounce_buffer_size_px; a>0; a--) {
				if ((no_pixels % (a*2))==0) {
					bounce_bytes = a*2;
					ESP_LOGW(TAG, "Frame buffer is not an integer multiple of bounce buffers.");
					ESP_LOGW(TAG, "Adjusted bounce buffer size from %d to %d pixels to fix this.",
							rgb_panel_config->bounce_buffer_size_px, bounce_bytes/2);
					break;
				}
			}
		}

		// DMA descriptors need to fit both bounce buffers
		num_dma_nodes = (bounce_bytes + DMA_DESCRIPTOR_BUFFER_MAX_SIZE - 1) / DMA_DESCRIPTOR_BUFFER_MAX_SIZE;
		num_dma_nodes = num_dma_nodes * 2; //as we have two bounce buffers
	}
	// DMA descriptors must be placed in internal SRAM (requested by DMA)
	rgb_panel = heap_caps_calloc(1, sizeof(esp_rgb_panel_t) + num_dma_nodes * sizeof(dma_descriptor_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
	ESP_GOTO_ON_FALSE(rgb_panel, ESP_ERR_NO_MEM, err, TAG, "no mem for rgb panel");
	rgb_panel->num_dma_nodes = num_dma_nodes;
	rgb_panel->panel_id = -1;
	// register to platform
	int panel_id = lcd_com_register_device(LCD_COM_DEVICE_TYPE_RGB, rgb_panel);
	ESP_GOTO_ON_FALSE(panel_id >= 0, ESP_ERR_NOT_FOUND, err, TAG, "no free rgb panel slot");
	rgb_panel->panel_id = panel_id;
	rgb_panel->bounce_buffer_size_bytes = bounce_bytes;
	// enable APB to access LCD registers
	periph_module_enable(lcd_periph_signals.panels[panel_id].module);
	periph_module_reset(lcd_periph_signals.panels[panel_id].module);
	// alloc frame buffer
    rgb_panel->fb_size = fb_size;
	bool alloc_from_psram = false;
	// fb_in_psram is only an option, if there's no PSRAM on board, we still alloc from SRAM
	if (rgb_panel_config->flags.fb_in_psram) {
#if CONFIG_SPIRAM_USE_MALLOC || CONFIG_SPIRAM_USE_CAPS_ALLOC
		if (esp_spiram_is_initialized()) {
			alloc_from_psram = true;
		}
#endif
	}
	size_t psram_trans_align = rgb_panel_config->psram_trans_align ? rgb_panel_config->psram_trans_align : 64;
	size_t sram_trans_align = rgb_panel_config->sram_trans_align ? rgb_panel_config->sram_trans_align : 4;
	
	if (!rgb_panel_config->on_bounce_empty) {
		//We need to allocate a framebuffer.
		if ( (alloc_from_psram) && (rgb_panel_config->fb == NULL) ) {
            ESP_LOGW(TAG, "Allocating framebuffer.");
			// the low level malloc function will help check the validation of alignment
			rgb_panel->fb = heap_caps_aligned_calloc(psram_trans_align, 1, fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            // rgb_panel->fb = m_malloc(fb_size, true); // for CircuitPython
            ESP_GOTO_ON_FALSE(rgb_panel->fb, ESP_ERR_NO_MEM, err, TAG, "no mem for frame buffer");
		} else {
			rgb_panel->fb = rgb_panel_config->fb;
            
		}
		
		rgb_panel->psram_trans_align = psram_trans_align;
		rgb_panel->sram_trans_align = sram_trans_align;
		rgb_panel->flags.fb_in_psram = alloc_from_psram;
	}
	if (rgb_panel->bounce_buffer_size_bytes) {
		//We need to allocate bounce buffers.
		rgb_panel->bounce_buffer[0] = heap_caps_aligned_calloc(sram_trans_align, 1, 
						rgb_panel->bounce_buffer_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
		rgb_panel->bounce_buffer[1] = heap_caps_aligned_calloc(sram_trans_align, 1, 
						rgb_panel->bounce_buffer_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
	}
	if (rgb_panel_config->on_bounce_empty) {
		rgb_panel->on_bounce_empty = rgb_panel_config->on_bounce_empty;
		rgb_panel->bounce_buffer_cb_user_ctx = rgb_panel_config->bounce_buffer_cb_user_ctx;
	}
	// initialize HAL layer, so we can call LL APIs later
	lcd_hal_init(&rgb_panel->hal, panel_id);
	// set peripheral clock resolution
	ret = lcd_rgb_panel_select_periph_clock(rgb_panel, rgb_panel_config->clk_src);
	ESP_GOTO_ON_ERROR(ret, err, TAG, "select periph clock failed");

    if (!rgb_panel_config->flags.relax_on_idle) {
        ESP_LOGW(TAG, "Installing interrupt service.");
    	// install interrupt service, (LCD peripheral shares the interrupt source with Camera by different mask)
    	int isr_flags = LCD_RGB_INTR_ALLOC_FLAGS | ESP_INTR_FLAG_SHARED;
    	ret = esp_intr_alloc_intrstatus(lcd_periph_signals.panels[panel_id].irq_id, isr_flags,
    									(uint32_t)lcd_ll_get_interrupt_status_reg(rgb_panel->hal.dev),
    									LCD_LL_EVENT_VSYNC_END, lcd_default_isr_handler, rgb_panel, &rgb_panel->intr);
    	ESP_GOTO_ON_ERROR(ret, err, TAG, "install interrupt failed");
    }
	lcd_ll_enable_interrupt(rgb_panel->hal.dev, LCD_LL_EVENT_VSYNC_END, false); // disable all interrupts
	lcd_ll_clear_interrupt_status(rgb_panel->hal.dev, UINT32_MAX); // clear pending interrupt
	// install DMA service
	rgb_panel->flags.stream_mode = !rgb_panel_config->flags.relax_on_idle;
	ret = lcd_rgb_panel_create_trans_link(rgb_panel);
	ESP_GOTO_ON_ERROR(ret, err, TAG, "install DMA failed");
	// configure GPIO
	ret = lcd_rgb_panel_configure_gpio(rgb_panel, rgb_panel_config);
	ESP_GOTO_ON_ERROR(ret, err, TAG, "configure GPIO failed");
	// fill other rgb panel runtime parameters
	memcpy(rgb_panel->data_gpio_nums, rgb_panel_config->data_gpio_nums, SOC_LCD_RGB_DATA_WIDTH);
	rgb_panel->timings = rgb_panel_config->timings;
	rgb_panel->data_width = rgb_panel_config->data_width;
	rgb_panel->disp_gpio_num = rgb_panel_config->disp_gpio_num;
	rgb_panel->flags.disp_en_level = !rgb_panel_config->flags.disp_active_low;
	rgb_panel->on_frame_trans_done = rgb_panel_config->on_frame_trans_done;
	rgb_panel->user_ctx = rgb_panel_config->user_ctx;
	// fill function table
	rgb_panel->base.del = rgb_panel_del;
	rgb_panel->base.reset = rgb_panel_reset;
	rgb_panel->base.init = rgb_panel_init;
	rgb_panel->base.draw_bitmap = rgb_panel_draw_bitmap;
	rgb_panel->base.disp_off = rgb_panel_disp_off;
	rgb_panel->base.invert_color = rgb_panel_invert_color;
	rgb_panel->base.mirror = rgb_panel_mirror;
	rgb_panel->base.swap_xy = rgb_panel_swap_xy;
	rgb_panel->base.set_gap = rgb_panel_set_gap;
	// return base class
	*ret_panel = &(rgb_panel->base);
	ESP_LOGD(TAG, "new rgb panel(%d) @%p, fb @%p, size=%zu", rgb_panel->panel_id, rgb_panel, rgb_panel->fb, rgb_panel->fb_size);
	return ESP_OK;

err:
	if (rgb_panel) {
		if (rgb_panel->panel_id >= 0) {
			periph_module_disable(lcd_periph_signals.panels[rgb_panel->panel_id].module);
			lcd_com_remove_device(LCD_COM_DEVICE_TYPE_RGB, rgb_panel->panel_id);
		}
		if (rgb_panel->fb) {
			free(rgb_panel->fb);
		}
		if (rgb_panel->dma_chan) {
			gdma_disconnect(rgb_panel->dma_chan);
			gdma_del_channel(rgb_panel->dma_chan);
		}
		if (rgb_panel->intr) {
			esp_intr_free(rgb_panel->intr);
		}
		if (rgb_panel->pm_lock) {
			esp_pm_lock_release(rgb_panel->pm_lock);
			esp_pm_lock_delete(rgb_panel->pm_lock);
		}
		free(rgb_panel);
	}
	return ret;
}


static esp_err_t rgb_panel_del(esp_lcd_panel_t *panel)
{
	esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
	int panel_id = rgb_panel->panel_id;
	gdma_disconnect(rgb_panel->dma_chan);
	gdma_del_channel(rgb_panel->dma_chan);
	esp_intr_free(rgb_panel->intr);
	periph_module_disable(lcd_periph_signals.panels[panel_id].module);
	lcd_com_remove_device(LCD_COM_DEVICE_TYPE_RGB, rgb_panel->panel_id);
	free(rgb_panel->fb);
	if (rgb_panel->pm_lock) {
		esp_pm_lock_release(rgb_panel->pm_lock);
		esp_pm_lock_delete(rgb_panel->pm_lock);
	}
	free(rgb_panel);
	ESP_LOGD(TAG, "del rgb panel(%d)", panel_id);
	return ESP_OK;
}

static esp_err_t rgb_panel_reset(esp_lcd_panel_t *panel)
{
	esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
	lcd_ll_fifo_reset(rgb_panel->hal.dev);
	lcd_ll_reset(rgb_panel->hal.dev);
	return ESP_OK;
}

static esp_err_t rgb_panel_init(esp_lcd_panel_t *panel)
{
	esp_err_t ret = ESP_OK;
	esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
	// configure clock
	lcd_ll_enable_clock(rgb_panel->hal.dev, true);
	// set PCLK frequency
	uint32_t pclk_prescale = rgb_panel->resolution_hz / rgb_panel->timings.pclk_hz;
	ESP_GOTO_ON_FALSE(pclk_prescale <= LCD_LL_CLOCK_PRESCALE_MAX, ESP_ERR_NOT_SUPPORTED, err, TAG,
					  "prescaler can't satisfy PCLK clock %uHz", rgb_panel->timings.pclk_hz);
	lcd_ll_set_pixel_clock_prescale(rgb_panel->hal.dev, pclk_prescale);
	rgb_panel->timings.pclk_hz = rgb_panel->resolution_hz / pclk_prescale;
	// pixel clock phase and polarity
	lcd_ll_set_clock_idle_level(rgb_panel->hal.dev, rgb_panel->timings.flags.pclk_idle_high);
	lcd_ll_set_pixel_clock_edge(rgb_panel->hal.dev, !rgb_panel->timings.flags.pclk_active_pos);
	// enable RGB mode and set data width
	lcd_ll_enable_rgb_mode(rgb_panel->hal.dev, true);
	lcd_ll_set_data_width(rgb_panel->hal.dev, rgb_panel->data_width);
	lcd_ll_set_phase_cycles(rgb_panel->hal.dev, 0, 0, 1); // enable data phase only
	// number of data cycles is controlled by DMA buffer size
	lcd_ll_enable_output_always_on(rgb_panel->hal.dev, true);
	// configure HSYNC, VSYNC, DE signal idle state level
	lcd_ll_set_idle_level(rgb_panel->hal.dev, !rgb_panel->timings.flags.hsync_idle_low,
						  !rgb_panel->timings.flags.vsync_idle_low, rgb_panel->timings.flags.de_idle_high);
	// configure blank region timing
	lcd_ll_set_blank_cycles(rgb_panel->hal.dev, 1, 1); // RGB panel always has a front and back blank (porch region)
	lcd_ll_set_horizontal_timing(rgb_panel->hal.dev, rgb_panel->timings.hsync_pulse_width,
								 rgb_panel->timings.hsync_back_porch, rgb_panel->timings.h_res,
								 rgb_panel->timings.hsync_front_porch);
	lcd_ll_set_vertical_timing(rgb_panel->hal.dev, rgb_panel->timings.vsync_pulse_width,
							   rgb_panel->timings.vsync_back_porch, rgb_panel->timings.v_res,
							   rgb_panel->timings.vsync_front_porch);
	// output hsync even in porch region
	lcd_ll_enable_output_hsync_in_porch_region(rgb_panel->hal.dev, true);
	// generate the hsync at the very begining of line
	lcd_ll_set_hsync_position(rgb_panel->hal.dev, 0);
	// restart flush by hardware has some limitation, instead, the driver will restart the flush in the VSYNC end interrupt by software
	lcd_ll_enable_auto_next_frame(rgb_panel->hal.dev, false);
	if (rgb_panel->flags.stream_mode) {
        // trigger interrupt on the end of frame
        lcd_ll_enable_interrupt(rgb_panel->hal.dev, LCD_LL_EVENT_VSYNC_END, true);
        // enable intr
        esp_intr_enable(rgb_panel->intr);
        // start transmission
		lcd_rgb_panel_start_transmission(rgb_panel);
	} 
	ESP_LOGD(TAG, "rgb panel(%d) start, pclk=%uHz", rgb_panel->panel_id, rgb_panel->timings.pclk_hz);
err:
	return ret;
}



static esp_err_t rgb_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
	esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
	if (rgb_panel->fb == NULL) {
		//Can't draw a bitmap to a non-existing framebuffer.
		//This happens when e.g. we use an external callback to refill the bounce buffers.
		return ESP_ERR_NOT_SUPPORTED;
	}
	assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
	// adjust the flush window by adding extra gap
	x_start += rgb_panel->x_gap;
	y_start += rgb_panel->y_gap;
	x_end += rgb_panel->x_gap;
	y_end += rgb_panel->y_gap;
	// round the boundary
	x_start = MIN(x_start, rgb_panel->timings.h_res);
	x_end = MIN(x_end, rgb_panel->timings.h_res);
	y_start = MIN(y_start, rgb_panel->timings.v_res);
	y_end = MIN(y_end, rgb_panel->timings.v_res);

	// convert the frame buffer to 3D array
	int bytes_per_pixel = rgb_panel->data_width / 8;
	int pixels_per_line = rgb_panel->timings.h_res;
	const uint8_t *from = (const uint8_t *)color_data;
	uint8_t (*to)[pixels_per_line][bytes_per_pixel] = (uint8_t (*)[pixels_per_line][bytes_per_pixel])rgb_panel->fb;
	// manipulate the frame buffer
	for (int j = y_start; j < y_end; j++) {
		for (int i = x_start; i < x_end; i++) {
			for (int k = 0; k < bytes_per_pixel; k++) {
				to[j][i][k] = *from++;
			}
		}
	}
	if (rgb_panel->flags.fb_in_psram && !rgb_panel->bounce_buffer_size_bytes) {
		// CPU writes data to PSRAM through DCache, data in PSRAM might not get updated, so write back
		// Note that if we use a bounce buffer, the data gets read by the CPU as well so no need to write back.
		Cache_WriteBack_Addr((uint32_t)&to[y_start][0][0], (y_end - y_start) * rgb_panel->timings.h_res * bytes_per_pixel);
	}

	// restart the new transmission
	if (!rgb_panel->flags.stream_mode) {
		lcd_rgb_panel_start_transmission(rgb_panel);
	}

	return ESP_OK;
}

static esp_err_t rgb_panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
	esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
	int panel_id = rgb_panel->panel_id;
	// inverting the data line by GPIO matrix
	for (int i = 0; i < rgb_panel->data_width; i++) {
		esp_rom_gpio_connect_out_signal(rgb_panel->data_gpio_nums[i], lcd_periph_signals.panels[panel_id].data_sigs[i],
										invert_color_data, false);
	}
	return ESP_OK;
}

static esp_err_t rgb_panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
	return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t rgb_panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
	return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t rgb_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
	esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
	rgb_panel->x_gap = x_gap;
	rgb_panel->x_gap = y_gap;
	return ESP_OK;
}

static esp_err_t rgb_panel_disp_off(esp_lcd_panel_t *panel, bool off)
{
	esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
	if (rgb_panel->disp_gpio_num < 0) {
		return ESP_ERR_NOT_SUPPORTED;
	}
	if (off) { // turn off screen
		gpio_set_level(rgb_panel->disp_gpio_num, !rgb_panel->flags.disp_en_level);
	} else { // turn on screen
		gpio_set_level(rgb_panel->disp_gpio_num, rgb_panel->flags.disp_en_level);
	}
	return ESP_OK;
}

static esp_err_t lcd_rgb_panel_configure_gpio(esp_rgb_panel_t *panel, const esp_lcd_rgb_panel_config_t *panel_config)
{
	int panel_id = panel->panel_id;
	// check validation of GPIO number
	bool valid_gpio = (panel_config->pclk_gpio_num >= 0);
	if (panel_config->de_gpio_num < 0) {
		// Hsync and Vsync are required in HV mode
		valid_gpio = valid_gpio && (panel_config->hsync_gpio_num >= 0) && (panel_config->vsync_gpio_num >= 0);
	}
	for (size_t i = 0; i < panel_config->data_width; i++) {
		valid_gpio = valid_gpio && (panel_config->data_gpio_nums[i] >= 0);
	}
	if (!valid_gpio) {
		return ESP_ERR_INVALID_ARG;
	}
	// connect peripheral signals via GPIO matrix
	for (size_t i = 0; i < panel_config->data_width; i++) {
		gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->data_gpio_nums[i]], PIN_FUNC_GPIO);
		gpio_set_direction(panel_config->data_gpio_nums[i], GPIO_MODE_OUTPUT);
		esp_rom_gpio_connect_out_signal(panel_config->data_gpio_nums[i],
										lcd_periph_signals.panels[panel_id].data_sigs[i], false, false);
	}
	if (panel_config->hsync_gpio_num >= 0) {
		gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->hsync_gpio_num], PIN_FUNC_GPIO);
		gpio_set_direction(panel_config->hsync_gpio_num, GPIO_MODE_OUTPUT);
		esp_rom_gpio_connect_out_signal(panel_config->hsync_gpio_num,
										lcd_periph_signals.panels[panel_id].hsync_sig, false, false);
	}
	if (panel_config->vsync_gpio_num >= 0) {
		gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->vsync_gpio_num], PIN_FUNC_GPIO);
		gpio_set_direction(panel_config->vsync_gpio_num, GPIO_MODE_OUTPUT);
		esp_rom_gpio_connect_out_signal(panel_config->vsync_gpio_num,
										lcd_periph_signals.panels[panel_id].vsync_sig, false, false);
	}
	gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->pclk_gpio_num], PIN_FUNC_GPIO);
	gpio_set_direction(panel_config->pclk_gpio_num, GPIO_MODE_OUTPUT);
	esp_rom_gpio_connect_out_signal(panel_config->pclk_gpio_num,
									lcd_periph_signals.panels[panel_id].pclk_sig, false, false);
	// DE signal might not be necessary for some RGB LCD
	if (panel_config->de_gpio_num >= 0) {
		gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->de_gpio_num], PIN_FUNC_GPIO);
		gpio_set_direction(panel_config->de_gpio_num, GPIO_MODE_OUTPUT);
		esp_rom_gpio_connect_out_signal(panel_config->de_gpio_num,
										lcd_periph_signals.panels[panel_id].de_sig, false, false);
	}
	// disp enable GPIO is optional
	if (panel_config->disp_gpio_num >= 0) {
		gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->disp_gpio_num], PIN_FUNC_GPIO);
		gpio_set_direction(panel_config->disp_gpio_num, GPIO_MODE_OUTPUT);
		esp_rom_gpio_connect_out_signal(panel_config->disp_gpio_num, SIG_GPIO_OUT_IDX, false, false);
	}
	return ESP_OK;
}

static esp_err_t lcd_rgb_panel_select_periph_clock(esp_rgb_panel_t *panel, lcd_clock_source_t clk_src)
{
	esp_err_t ret = ESP_OK;
	// force to use integer division, as fractional division might lead to clock jitter
	lcd_ll_set_group_clock_src(panel->hal.dev, clk_src, LCD_PERIPH_CLOCK_PRE_SCALE, 0, 0);
	switch (clk_src) {
	case LCD_CLK_SRC_PLL160M:
		panel->resolution_hz = 160000000 / LCD_PERIPH_CLOCK_PRE_SCALE;
#if CONFIG_PM_ENABLE
		ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "rgb_panel", &panel->pm_lock);
		ESP_RETURN_ON_ERROR(ret, TAG, "create ESP_PM_APB_FREQ_MAX lock failed");
		// hold the lock during the whole lifecycle of RGB panel
		esp_pm_lock_acquire(panel->pm_lock);
		ESP_LOGD(TAG, "installed ESP_PM_APB_FREQ_MAX lock and hold the lock during the whole panel lifecycle");
#endif
		break;
	case LCD_CLK_SRC_XTAL:
		panel->resolution_hz = rtc_clk_xtal_freq_get() * 1000000 / LCD_PERIPH_CLOCK_PRE_SCALE;
		break;
	default:
		ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG,	"unsupported clock source: %d", clk_src);
		break;
	}
	return ret;
}

static IRAM_ATTR bool lcd_rgb_panel_fill_bounce_buffer(esp_rgb_panel_t *panel, uint8_t *buffer)
{
	bool need_yield = false;
	if (panel->on_bounce_empty) {
		//We don't have a framebuffer here; we need to call a callback to refill the bounce buffer
		//for us.
		need_yield=panel->on_bounce_empty((void*)buffer, panel->bounce_pos_px, 
					panel->bounce_buffer_size_bytes, panel->bounce_buffer_cb_user_ctx);
	} else {
		//We do have a framebuffer; copy from there.
		memcpy(buffer, &panel->fb[panel->bounce_pos_px*2], panel->bounce_buffer_size_bytes);
		//We don't need the bytes we copied from psram anymore.
		//Make sure that if anything happened to have changed (because the line already was in cache) we write
		//the data back
		Cache_WriteBack_Addr((uint32_t)&panel->fb[panel->bounce_pos_px*2], panel->bounce_buffer_size_bytes);
		//Invalidate the data. Note: possible race: perhaps something can squeeze a byte between this and the
		//writeback, in which case that data gets discarded. Probably need to warn people to only write to the
		//fb on one core... which you probably want to do anyway given the bw limits of the bus.
		Cache_Invalidate_Addr((uint32_t)&panel->fb[panel->bounce_pos_px*2], panel->bounce_buffer_size_bytes);
	}
	panel->bounce_pos_px+=panel->bounce_buffer_size_bytes/2;
	//If the bounce pos is larger than the framebuffer size, wrap around so the next isr starts pre-loading
	//the next frame.
	if (panel->bounce_pos_px >= panel->fb_size/2) {
		panel->bounce_pos_px=0;
	}
	if (!panel->on_bounce_empty) {
		//Preload the next bit of buffer into psram.
		Cache_Start_DCache_Preload((uint32_t)&panel->fb[panel->bounce_pos_px*2],
					panel->bounce_buffer_size_bytes, 0);
	}
	return need_yield;
}

static IRAM_ATTR bool lcd_rgb_panel_eof_handler(gdma_channel_handle_t dma_chan, gdma_event_data_t *event_data, void *user_data)
{
	esp_rgb_panel_t *panel = (esp_rgb_panel_t*)user_data;
	dma_descriptor_t *desc = (dma_descriptor_t*)event_data->tx_eof_desc_addr;
	//Figure out which bounce buffer to write to.
	//Note: what we receive is the *last* descriptor of this bounce buffer. 
	if (desc==&panel->dma_nodes[panel->num_dma_nodes - 1]) {
		return lcd_rgb_panel_fill_bounce_buffer(panel, panel->bounce_buffer[1]);
	} else {
		return lcd_rgb_panel_fill_bounce_buffer(panel, panel->bounce_buffer[0]);
	}
	return false; //never reached
}

static esp_err_t lcd_rgb_panel_create_trans_link(esp_rgb_panel_t *panel)
{
	esp_err_t ret = ESP_OK;
	if (panel->bounce_buffer_size_bytes == 0) {
		// Create DMA descriptors for main framebuffer.
		// chain DMA descriptors
		for (int i = 0; i < panel->num_dma_nodes; i++) {
			panel->dma_nodes[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
			panel->dma_nodes[i].next = &panel->dma_nodes[i + 1];
		}
		//loop end back to start
		panel->dma_nodes[panel->num_dma_nodes - 1].next = &panel->dma_nodes[0];
		// mount the frame buffer to the DMA descriptors
		lcd_com_mount_dma_data(panel->dma_nodes, panel->fb, panel->fb_size);
	} else {
		//Create DMA descriptors for bounce buffers
		for (int i = 0; i < panel->num_dma_nodes; i++) {
			panel->dma_nodes[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
			panel->dma_nodes[i].next = &panel->dma_nodes[i + 1];
		}
		//loop end back to start
		panel->dma_nodes[panel->num_dma_nodes - 1].next = &panel->dma_nodes[0];
		//set eof on end of 1st and 2nd bounce buffer
		panel->dma_nodes[(panel->num_dma_nodes/2) - 1].dw0.suc_eof=1;
		panel->dma_nodes[panel->num_dma_nodes - 1].dw0.suc_eof=1;
		// mount the bounce buffers to the DMA descriptors
		lcd_com_mount_dma_data(&panel->dma_nodes[0], panel->bounce_buffer[0], panel->bounce_buffer_size_bytes);
		lcd_com_mount_dma_data(&panel->dma_nodes[(panel->num_dma_nodes/2)], panel->bounce_buffer[1], panel->bounce_buffer_size_bytes);
	}
	// alloc DMA channel and connect to LCD peripheral
	gdma_channel_alloc_config_t dma_chan_config = {
		.direction = GDMA_CHANNEL_DIRECTION_TX,
	};
	ret = gdma_new_channel(&dma_chan_config, &panel->dma_chan);
	ESP_GOTO_ON_ERROR(ret, err, TAG, "alloc DMA channel failed");
	gdma_connect(panel->dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
	gdma_transfer_ability_t ability = {
		.psram_trans_align = panel->psram_trans_align,
		.sram_trans_align = panel->sram_trans_align,
	};
	gdma_set_transfer_ability(panel->dma_chan, &ability);
	if (panel->bounce_buffer_size_bytes != 0) {
		// register callback to re-fill bounce buffers once they're fully sent
		gdma_tx_event_callbacks_t cbs={0};
		cbs.on_trans_eof = lcd_rgb_panel_eof_handler;
		gdma_register_tx_event_callbacks(panel->dma_chan, &cbs, panel);
	}
	// the start of DMA should be prior to the start of LCD engine
	gdma_start(panel->dma_chan, (intptr_t)panel->dma_nodes);

err:
	return ret;
}

static void lcd_rgb_panel_restart_transmission(esp_rgb_panel_t *panel)
{
	if (panel->bounce_buffer_size_bytes != 0) {
		//Catch de-synced framebuffer and reset if needed.
		if (panel->bounce_pos_px > panel->bounce_buffer_size_bytes) panel->bounce_pos_px=0;
		//Pre-fill bounce buffer 0, if the EOF ISR didn't do that already
		if (panel->bounce_pos_px < panel->bounce_buffer_size_bytes/2) {
			lcd_rgb_panel_fill_bounce_buffer(panel, panel->bounce_buffer[0]);
		}
	}

	gdma_reset(panel->dma_chan);
    // esp_rom_delay_us(1); // ** added 
	lcd_ll_fifo_reset(panel->hal.dev); 
    // esp_rom_delay_us(1); // ** added 
	gdma_start(panel->dma_chan, (intptr_t)panel->dma_nodes);
    esp_rom_delay_us(1); // ** added - tried 1 and 5 us

	if (panel->bounce_buffer_size_bytes != 0) {
		//Fill 2nd bounce buffer while 1st is being sent out, if needed.
		if (panel->bounce_pos_px < panel->bounce_buffer_size_bytes) {
			lcd_rgb_panel_fill_bounce_buffer(panel, panel->bounce_buffer[0]);
		}
	}
}

static void lcd_rgb_panel_start_transmission(esp_rgb_panel_t *rgb_panel)
{
	// reset FIFO of DMA and LCD, incase there remains old frame data
	gdma_reset(rgb_panel->dma_chan);
	lcd_ll_stop(rgb_panel->hal.dev);
	lcd_ll_fifo_reset(rgb_panel->hal.dev);

	//pre-fill bounce buffers if needed
	if (rgb_panel->bounce_buffer_size_bytes != 0) {
        rgb_panel->bounce_pos_px = 0;
		lcd_rgb_panel_fill_bounce_buffer(rgb_panel, rgb_panel->bounce_buffer[0]);
		lcd_rgb_panel_fill_bounce_buffer(rgb_panel, rgb_panel->bounce_buffer[1]);
		
	}

	gdma_start(rgb_panel->dma_chan, (intptr_t)rgb_panel->dma_nodes);
	// delay 1us is sufficient for DMA to pass data to LCD FIFO
	// in fact, this is only needed when LCD pixel clock is set too high
	esp_rom_delay_us(1);
	// start LCD engine
	lcd_ll_enable_auto_next_frame(rgb_panel->hal.dev, rgb_panel->flags.stream_mode);
	lcd_ll_start(rgb_panel->hal.dev);
}

IRAM_ATTR static void lcd_default_isr_handler(void *args)
{
	esp_rgb_panel_t *rgb_panel = (esp_rgb_panel_t *)args;
	bool need_yield = false;

	uint32_t intr_status = lcd_ll_get_interrupt_status(rgb_panel->hal.dev);
	lcd_ll_clear_interrupt_status(rgb_panel->hal.dev, intr_status);
	if (intr_status & LCD_LL_EVENT_VSYNC_END) {
		// call user registered callback
		if (rgb_panel->on_frame_trans_done) {
			if (rgb_panel->on_frame_trans_done(&rgb_panel->base, NULL, rgb_panel->user_ctx)) {
				need_yield = true;
			}
		}
		if (rgb_panel->flags.stream_mode) {
			// In some cases, there can be a GDMA bandwidth issue that de-syncs the LCD and GDMA 
			// states, as in the GDMA provides a pixel earlier or later than what the LCD peripheral
			// wants to output. To compensate for that, we reset the DMA in the VBlank because we 
			// know the LCD will be requesting pixel (0,0) next.
			// ToDo: Only reset DMA when this interrupt is not late!
			lcd_rgb_panel_restart_transmission(rgb_panel);
		}
	}

	if (need_yield) {
		portYIELD_FROM_ISR();
	}
}

uint8_t * rgb_panel_get_buffer(esp_lcd_panel_t *panel) {
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);

    return rgb_panel->fb;
}

size_t rgb_panel_get_buffer_size(esp_lcd_panel_t *panel) {
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    return rgb_panel->fb_size;
}

esp_err_t rgb_panel_flush_buffer(esp_lcd_panel_t *panel) {
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);

    // if (rgb_panel->flags.fb_in_psram) {
    //     // CPU writes data to PSRAM through DCache, data in PSRAM might not get updated, so write back

    //     //May not be necessary with the bounce_buffer ***
    //     Cache_WriteBack_Addr((uint32_t) rgb_panel->fb, rgb_panel->fb_size);
    // }

    lcd_rgb_panel_start_transmission(panel); // trial *****

    return ESP_OK;
}
