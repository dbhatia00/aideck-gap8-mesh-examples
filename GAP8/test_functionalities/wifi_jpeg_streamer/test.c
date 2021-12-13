#include "pmsis.h"

#include "bsp/camera/himax.h"
#include "bsp/buffer.h"
#include "gaplib/jpeg_encoder.h"
#include "stdio.h"

#include "com.h"
#include "routing_info.h"

// This file should be created manually and not committed (part of ignore)
// It should contain the following:
// static const char ssid[] = "YourSSID";
// static const char passwd[] = "YourWiFiKey";
#include "wifi_credentials.h"

#define IMG_ORIENTATION 0x0101
#define CAM_WIDTH 324
#define CAM_HEIGHT 244

static pi_task_t task1;
static unsigned char *imgBuff0;
static struct pi_device camera;
static pi_buffer_t buffer;

static EventGroupHandle_t evGroup;
#define CAPTURE_DONE_BIT (1 << 0)

static int open_pi_camera_himax(struct pi_device *device)
{
  struct pi_himax_conf cam_conf;

  pi_himax_conf_init(&cam_conf);

  cam_conf.format = PI_CAMERA_QVGA;

  pi_open_from_conf(device, &cam_conf);
  if (pi_camera_open(device))
    return -1;

  // rotate image
  pi_camera_control(&camera, PI_CAMERA_CMD_START, 0);
  uint8_t set_value = 3;
  uint8_t reg_value;
  pi_camera_reg_set(&camera, IMG_ORIENTATION, &set_value);
  pi_time_wait_us(1000000);
  pi_camera_reg_get(&camera, IMG_ORIENTATION, &reg_value);
  if (set_value != reg_value)
  {
    printf("Failed to rotate camera image\n");
    return -1;
  }
  pi_camera_control(&camera, PI_CAMERA_CMD_STOP, 0);

  pi_camera_control(device, PI_CAMERA_CMD_AEG_INIT, 0);

  return 0;
}

#define LED_PIN 2

static pi_device_t led_gpio_dev;

static int wifiConnected = 0;
static int wifiClientConnected = 0;

static routed_packet_t rxp;
void rx_task(void *parameters)
{
  while (1)
  {
    com_read(&rxp);

    switch (rxp.data[0])
    {
    case 0x21:
      printf("Wifi connected (%u.%u.%u.%u)\n", rxp.data[1], rxp.data[2],
                                               rxp.data[3], rxp.data[4]);
      wifiConnected = 1;
      break;
    case 0x23:
      printf("Wifi client connection status: %u\n", rxp.data[1]);
      wifiClientConnected = rxp.data[1];
      break;
    }
  }
}

static void capture_done_cb(void *arg)
{
  xEventGroupSetBits(evGroup, CAPTURE_DONE_BIT);
}

typedef struct
{
  uint8_t magic;
  uint16_t width;
  uint16_t height;
  uint8_t depth;
  uint8_t type;
  uint32_t size;
} __attribute__((packed)) img_header_t;

static uint32_t start;
static uint32_t end;

static jpeg_encoder_t jpeg_encoder;

typedef enum
{
  RAW_ENCODING = 1,
  JPEG_ENCODING = 2
} __attribute__((packed)) StreamerMode_t;

pi_buffer_t header;
int headerSize;
pi_buffer_t footer;
int footerSize;
pi_buffer_t jpeg_data;
uint32_t jpegSize;

static StreamerMode_t streamerMode = RAW_ENCODING;

static routed_packet_t txp;

void camera_task(void *parameters)
{
  vTaskDelay(2000);

  printf("Sending wifi stuff...\n");
  txp.len = 2 + sizeof(ssid);
  txp.dst = MAKE_ROUTE(ESP32, WIFI_CTRL);
  txp.src = MAKE_ROUTE(GAP8, WIFI_CTRL);
  txp.data[0] = 0x10; // Set SSID
  memcpy(&txp.data[1], ssid, sizeof(ssid));
  com_write(&txp);

  txp.len = 2 + sizeof(passwd);
  txp.data[0] = 0x11; // Set passwd
  memcpy(&txp.data[1], passwd, sizeof(passwd));
  com_write(&txp);

  txp.len = 3;
  txp.data[0] = 0x20; // Connect wifi
  com_write(&txp);

  printf("Starting camera task...\n");
  uint32_t resolution = CAM_WIDTH * CAM_HEIGHT;
  uint32_t imgSize = resolution * sizeof(unsigned char);
  imgBuff0 = (unsigned char *)pmsis_l2_malloc(imgSize);
  if (imgBuff0 == NULL)
  {
    printf("Failed to allocate Memory for Image \n");
    return 1;
  }
  printf("Allocated memory for image: %u bytes\n", imgSize);

  if (open_pi_camera_himax(&camera))
  {
    printf("Failed to open camera\n");
    return -1;
  }
  printf("Camera is open\n");

  struct jpeg_encoder_conf enc_conf;
  jpeg_encoder_conf_init(&enc_conf);
  enc_conf.width = CAM_WIDTH;
  enc_conf.height = CAM_HEIGHT;
  enc_conf.flags = 0; // Move this to the cluster

  if (jpeg_encoder_open(&jpeg_encoder, &enc_conf))
  {
    printf("Failed initialize JPEG encoder\n");
    return -1;
  }

  printf("JPEG encoder initialized\n");

  pi_buffer_init(&buffer, PI_BUFFER_TYPE_L2, imgBuff0);
  pi_buffer_set_format(&buffer, CAM_WIDTH, CAM_HEIGHT, 1, PI_BUFFER_FORMAT_GRAY);

  header.size = 1024;
  header.data = pmsis_l2_malloc(1024);

  footer.size = 10;
  footer.data = pmsis_l2_malloc(10);

  // This must fit the full encoded JPEG
  jpeg_data.size = 1024 * 15;
  jpeg_data.data = pmsis_l2_malloc(1024 * 15);

  // Check malloc!

  jpeg_encoder_header(&jpeg_encoder, &header, &headerSize);
  printf("JPEG header size is %u\n", headerSize);
  jpeg_encoder_footer(&jpeg_encoder, &footer, &footerSize);
  printf("JPEG footer size is %u\n", footerSize);

  pi_camera_control(&camera, PI_CAMERA_CMD_STOP, 0);
  while (1)
  {
    if (wifiClientConnected == 1)
    {
      start = xTaskGetTickCount();
      pi_camera_capture_async(&camera, imgBuff0, resolution, pi_task_callback(&task1, capture_done_cb, NULL));
      pi_camera_control(&camera, PI_CAMERA_CMD_START, 0);
      xEventGroupWaitBits(evGroup, CAPTURE_DONE_BIT, pdTRUE, pdFALSE, (TickType_t)portMAX_DELAY);
      pi_camera_control(&camera, PI_CAMERA_CMD_STOP, 0);
      end = xTaskGetTickCount();
      printf("Captured in %u ms\n", end - start);

      if (streamerMode == JPEG_ENCODING)
      {
        //jpeg_encoder_process_async(&jpeg_encoder, &buffer, &jpeg_data, pi_task_callback(&task1, encoding_done_cb, NULL));
        //xEventGroupWaitBits(evGroup, JPEG_ENCODING_DONE_BIT, pdTRUE, pdFALSE, (TickType_t)portMAX_DELAY);
        //jpeg_encoder_process_status(&jpegSize, NULL);
        start = xTaskGetTickCount();
        jpeg_encoder_process(&jpeg_encoder, &buffer, &jpeg_data, &jpegSize);
        end = xTaskGetTickCount();
        printf("Encoded in %u ms (size is %u)\n", end - start, jpegSize);

        txp.dst = MAKE_ROUTE(ESP32, WIFI_DATA);
        txp.src = MAKE_ROUTE(GAP8, WIFI_DATA);

        uint32_t imgSize = headerSize + jpegSize + footerSize;

        // First send information about the image
        img_header_t *imgHeader = (img_header_t *)txp.data;
        imgHeader->magic = 0xBC;
        imgHeader->width = CAM_WIDTH;
        imgHeader->height = CAM_HEIGHT;
        imgHeader->depth = 1;
        imgHeader->type = 1;
        imgHeader->size = imgSize;
        txp.len = sizeof(img_header_t) + 2;
        com_write(&txp);

        int i = 0;
        int part = 0;
        int offset = 0;
        int size = 0;

        start = xTaskGetTickCount();
        // First send header
        memcpy(txp.data, header.data, headerSize);
        txp.len = headerSize + 2; // + 2 for header
        com_write(&txp);

        do
        {
          offset = part * sizeof(txp.data);
          size = sizeof(txp.data);
          if (offset + size > jpegSize)
          {
            size = jpegSize - offset;
          }
          memcpy(txp.data, &jpeg_data.data[offset], sizeof(txp.data));
          //printf("Copied from %u (size is %u)\n", offset, size);
          txp.len = size + 2; // + 2 for header
          com_write(&txp);
          part++;
        } while (size == sizeof(txp.data));

        memcpy(txp.data, footer.data, footerSize);
        txp.len = footerSize + 2; // + 2 for header
        com_write(&txp);

        end = xTaskGetTickCount();
        printf("Sent in %u\n", end - start);
      }
      else
      {
        start = xTaskGetTickCount();

        txp.dst = MAKE_ROUTE(ESP32, WIFI_DATA);
        txp.src = MAKE_ROUTE(GAP8, WIFI_DATA);

        // First send information about the image
        img_header_t *header = (img_header_t *)txp.data;
        header->magic = 0xBC;
        header->width = CAM_WIDTH;
        header->height = CAM_HEIGHT;
        header->depth = 1;
        header->type = 0;
        header->size = imgSize;
        txp.len = sizeof(img_header_t) + 2;
        com_write(&txp);

        int i = 0;
        int part = 0;
        int offset = 0;
        int size = 0;

        do
        {
          offset = part * sizeof(txp.data);
          size = sizeof(txp.data);
          if (offset + size > imgSize)
          {
            size = imgSize - offset;
          }
          memcpy(txp.data, &imgBuff0[offset], sizeof(txp.data));
          //printf("Copied from %u (size is %u)\n", offset, size);
          txp.len = size + 2; // + 2 for header
          com_write(&txp);
          part++;
        } while (size == sizeof(txp.data));
        //printf("Finished sending image\n");
        end = xTaskGetTickCount();
        printf("Sent in %u\n", end - start);
        vTaskDelay(10);
      }
    }
    else
    {
      //printf("Client is not connected, hold off\n");
      vTaskDelay(10);
    }
  }
}

void hb_task(void *parameters)
{
  (void)parameters;
  char *taskname = pcTaskGetName(NULL);

  // Initialize the LED pin
  pi_gpio_pin_configure(&led_gpio_dev, LED_PIN, PI_GPIO_OUTPUT);

  const TickType_t xDelay = 500 / portTICK_PERIOD_MS;

  while (1)
  {
    pi_gpio_pin_write(&led_gpio_dev, LED_PIN, 1);
    vTaskDelay(xDelay);
    pi_gpio_pin_write(&led_gpio_dev, LED_PIN, 0);
    vTaskDelay(xDelay);
  }
}

void start_bootloader(void)
{
  struct pi_uart_conf conf;
  struct pi_device device;
  pi_uart_conf_init(&conf);
  conf.baudrate_bps = 115200;

  pi_open_from_conf(&device, &conf);
  if (pi_uart_open(&device))
  {
    printf("[UART] open failed !\n");
    pmsis_exit(-1);
  }

  printf("\nStarting up!\n");
  printf("FC at %u MHz\n", pi_freq_get(PI_FREQ_DOMAIN_FC) / 1000000);

  printf("Starting up tasks...\n");

  evGroup = xEventGroupCreate();

  BaseType_t xTask;

  xTask = xTaskCreate(hb_task, "hb_task", configMINIMAL_STACK_SIZE * 2,
                      NULL, tskIDLE_PRIORITY + 1, NULL);
  if (xTask != pdPASS)
  {
    printf("HB task did not start !\n");
    pmsis_exit(-1);
  }

  com_init();

  xTask = xTaskCreate(camera_task, "camera_task", configMINIMAL_STACK_SIZE * 4,
                      NULL, tskIDLE_PRIORITY + 1, NULL);

  if (xTask != pdPASS)
  {
    printf("Camera task did not start !\n");
    pmsis_exit(-1);
  }

  xTask = xTaskCreate(rx_task, "rx_task", configMINIMAL_STACK_SIZE * 2,
                      NULL, tskIDLE_PRIORITY + 1, NULL);

  if (xTask != pdPASS)
  {
    printf("RX task did not start !\n");
    pmsis_exit(-1);
  }

  while (1)
  {
    pi_yield();
  }
}

int main(void)
{
  pi_bsp_init();

  // Increase the FC freq to 250 MHz
  pi_freq_set(PI_FREQ_DOMAIN_FC, 250000000);
  pi_pmu_voltage_set(PI_PMU_DOMAIN_FC, 1200);

  return pmsis_kickoff((void *)start_bootloader);
}