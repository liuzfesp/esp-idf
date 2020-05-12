/* Iperf Example - wifi commands

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "cmd_decl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "iperf.h"
#include "esp_private/wifi.h"

#define iperf_tolower(c)             ((c) | 0x20 )
#define iperf_in_range(c, lo, up)    ((uint8_t)c >= lo && (uint8_t)c <= up)
#define iperf_isdigit(c)             (iperf_in_range(c, '0', '9'))
#define iperf_isxdigit(c)            (iperf_isdigit(c) || iperf_in_range(c, 'a', 'f') || iperf_in_range(c, 'A', 'F'))

#ifndef REG_WRITE
#define REG_WRITE(_w,_v)             (*(volatile uint32_t *)(_w)) = (_v)
#endif
#ifndef REG_READ
#define REG_READ(_r)                 (*(volatile uint32_t *)(_r))
#endif

typedef struct {
    struct arg_str *ip;
    struct arg_lit *server;
    struct arg_lit *udp;
    struct arg_int *port;
    struct arg_int *interval;
    struct arg_int *time;
    struct arg_str *ip_tos;
    struct arg_int *tcp_win_size;
    struct arg_lit *abort;
    struct arg_end *end;
} wifi_iperf_t;

typedef struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_args_t;

typedef struct {
    struct arg_str *ssid;
    struct arg_end *end;
} wifi_scan_arg_t;

typedef struct {
    struct arg_str *rate;
    struct arg_end *end;
} wifi_fix_rate_t;

typedef struct {
    struct arg_str *type;
    struct arg_end *end;
} wifi_stats_args_t;

typedef struct {
    struct arg_lit *get_max_tx_power;
    struct arg_int *set_max_tx_power;
    struct arg_end *end;
} wifi_tpw_arg_t;

typedef struct {
    struct arg_dbl *read_reg;
    struct arg_dbl *write_reg;
    struct arg_dbl *value;
    struct arg_end *end;
} wifi_reg_arg_t;

typedef struct {
    struct arg_str *get_interface;
    struct arg_str *set_interface;
    struct arg_str *protocol;
    struct arg_end *end;
} wifi_protocol_arg_t;

typedef struct {
    struct arg_str *get_interface;
    struct arg_str *set_interface;
    struct arg_str *bandwidth;
    struct arg_end *end;
} wifi_bandwidth_arg_t;

static wifi_iperf_t iperf_args;
static wifi_fix_rate_t fix_rate_args;
static wifi_stats_args_t stats_args;
static wifi_args_t sta_args;
static wifi_scan_arg_t scan_args;
static wifi_args_t ap_args;
static wifi_reg_arg_t reg_args;
static wifi_tpw_arg_t tpw_args;
static wifi_protocol_arg_t pro_args;
static wifi_bandwidth_arg_t bwd_args;

static bool reconnect = true;
static const char *TAG="cmd_wifi";
static esp_netif_t *netif_ap = NULL;
static esp_netif_t *netif_sta = NULL;

static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const int DISCONNECTED_BIT = BIT1;

static void scan_done_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    uint16_t sta_number = 0;
    uint8_t i;
    wifi_ap_record_t *ap_list_buffer;

    esp_wifi_scan_get_ap_num(&sta_number);
    ap_list_buffer = malloc(sta_number * sizeof(wifi_ap_record_t));
    if (ap_list_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to malloc buffer to print scan results");
        return;
    }

    if (esp_wifi_scan_get_ap_records(&sta_number,(wifi_ap_record_t *)ap_list_buffer) == ESP_OK) {
        for(i=0; i<sta_number; i++) {
            ESP_LOGI(TAG, "[%s][rssi=%d]", ap_list_buffer[i].ssid, ap_list_buffer[i].rssi);
        }
    }
    free(ap_list_buffer);
    ESP_LOGI(TAG, "sta scan done");
}

static void got_ip_handler(void* arg, esp_event_base_t event_base,
                           int32_t event_id, void* event_data)
{
    xEventGroupClearBits(wifi_event_group, DISCONNECTED_BIT);
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (reconnect) {
        ESP_LOGI(TAG, "sta disconnect, reconnect...");
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "sta disconnect");
    }
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    xEventGroupSetBits(wifi_event_group, DISCONNECTED_BIT);
}


void initialise_wifi(void)
{
    static bool initialized = false;

    if (initialized) {
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_create_default() );
    netif_ap = esp_netif_create_default_wifi_ap();
    assert(netif_ap);
    netif_sta = esp_netif_create_default_wifi_sta();
    assert(netif_sta);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        WIFI_EVENT_SCAN_DONE,
                                                        &scan_done_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        WIFI_EVENT_STA_DISCONNECTED,
                                                        &disconnect_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &got_ip_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL) );
    ESP_ERROR_CHECK(esp_wifi_start() );
    initialized = true;
}

static bool wifi_cmd_sta_join(const char* ssid, const char* pass)
{
    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);

    wifi_config_t wifi_config = { 0 };

    strlcpy((char*) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    if (pass) {
        strlcpy((char*) wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    }

    if (bits & CONNECTED_BIT) {
        reconnect = false;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        ESP_ERROR_CHECK( esp_wifi_disconnect() );
        xEventGroupWaitBits(wifi_event_group, DISCONNECTED_BIT, 0, 1, portTICK_RATE_MS);
    }

    reconnect = true;
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_connect() );

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 5000/portTICK_RATE_MS);

    return true;
}

static int wifi_cmd_sta(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &sta_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, sta_args.end, argv[0]);
        return 1;
    }

    ESP_LOGI(TAG, "sta connecting to '%s'", sta_args.ssid->sval[0]);
    wifi_cmd_sta_join(sta_args.ssid->sval[0], sta_args.password->sval[0]);
    return 0;
}

static bool wifi_cmd_sta_scan(const char* ssid)
{
    wifi_scan_config_t scan_config = { 0 };
    scan_config.ssid = (uint8_t *) ssid;

    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_scan_start(&scan_config, false) );

    return true;
}

static int wifi_cmd_scan(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &scan_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, scan_args.end, argv[0]);
        return 1;
    }

    ESP_LOGI(TAG, "sta start to scan");
    if ( scan_args.ssid->count == 1 ) {
        wifi_cmd_sta_scan(scan_args.ssid->sval[0]);
    } else {
        wifi_cmd_sta_scan(NULL);
    }
    return 0;
}

static int wifi_cmd_reg(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &reg_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, reg_args.end, argv[0]);
        return 0;
    }
    
    if (reg_args.read_reg->count == 1) {
        uint32_t read_addr = 0xFFFFFFFF;
        read_addr = reg_args.read_reg->dval[0];
        ESP_LOGI(TAG, "REGISTER:0x%08X,0x%08X\n\n", read_addr, REG_READ(read_addr));
    } else {
        if (reg_args.value->count == 0) {
            ESP_LOGE(TAG, "Please add the register parameter value you want to write");
            return 0;
        }
        uint32_t write_addr = 0xFFFFFFFF;
        uint32_t value = 0;

        write_addr = reg_args.write_reg->dval[0];
        value = reg_args.value->dval[0];
        REG_WRITE(write_addr,value);
        ESP_LOGI(TAG, "Write Register:0x%08X,0x%08X\n\n", write_addr, value);
    }
       
    return 0;
}

static int wifi_cmd_tpw(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &tpw_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, tpw_args.end, argv[0]);
        return 0;
    }
     
    int8_t tx_power = 0;
    esp_err_t err;
    if (tpw_args.set_max_tx_power->count == 0) {
         err = esp_wifi_get_max_tx_power(&tx_power);
         if (err != ESP_OK) {
             ESP_LOGE(TAG, "Get max tx power ERROR");
         } else {
             ESP_LOGI(TAG, "Get max tx power is %d", tx_power);
         }
     } else {
         tx_power = tpw_args.set_max_tx_power->ival[0];
         err = esp_wifi_set_max_tx_power(tx_power);
         if (err != ESP_OK) {
             ESP_LOGE(TAG, "Set max tx power ERROR");
         } else {
             ESP_LOGI(TAG, "Set max tx power SUCCESS");
         }
     }

    return 0;
}

static wifi_interface_t wifi_interface(const char* interface)
{
    wifi_interface_t ifx = ESP_IF_MAX;

    if (!memcmp(interface, "sta", sizeof("sta"))) {
        ifx = ESP_IF_WIFI_STA;
    } else if (!memcmp(interface, "ap", sizeof("ap"))) {
        ifx = ESP_IF_WIFI_AP;
    } else if (!memcmp(interface, "eth", sizeof("eth"))) {
        ifx = ESP_IF_ETH;
    } else {
        ESP_LOGE(TAG, "Invalid parameter");
    }

    return ifx;

}

static int wifi_cmd_pro(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &pro_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, pro_args.end, argv[0]);
        return 0;
    }

    esp_err_t err;
    uint8_t bitmap;
    wifi_interface_t ifx;

    if (pro_args.set_interface->count == 1) {
        ifx = wifi_interface(pro_args.set_interface->sval[0]);

        if (!memcmp(pro_args.protocol->sval[0], "bgn", sizeof("bgn"))) {
            bitmap = WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N;
        } else if (!memcmp(pro_args.protocol->sval[0], "bg", sizeof("bg"))) {
            bitmap = WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G;
        } else if (!memcmp(pro_args.protocol->sval[0], "b", sizeof("b"))) {
            bitmap = WIFI_PROTOCOL_11B;
        } else {
            ESP_LOGE(TAG, "Invalid parameter");
        }

        err = esp_wifi_set_protocol(ifx, bitmap);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Set protocol ERROR");
        } else {
            ESP_LOGI(TAG, "Set protocol SUCCESS");
        }
    } else {
        ifx = wifi_interface(pro_args.get_interface->sval[0]);

        err = esp_wifi_get_protocol(ifx, &bitmap);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Get protocol ERROR");
        } else {
            ESP_LOGI(TAG, "Current WiFi protocol is %s", (bitmap == (WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N)) ?
                    "BGN" : (bitmap == (WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G) ? "BG" : "B"));
        }
    }

    return 0;

}

static int wifi_cmd_bwd(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &bwd_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, bwd_args.end, argv[0]);
        return 0;
    }

    esp_err_t err;
    wifi_bandwidth_t bandwidth;
    wifi_interface_t ifx = ESP_IF_WIFI_STA;

    if (bwd_args.set_interface->count == 1) {
        ifx = wifi_interface(bwd_args.set_interface->sval[0]);
        
        if (!memcmp(bwd_args.bandwidth->sval[0], "ht20", sizeof("bt20"))) {
            bandwidth = WIFI_BW_HT20;
        } else if (!memcmp(bwd_args.bandwidth->sval[0], "ht40", sizeof("bt40"))) {
            bandwidth = WIFI_BW_HT40;
        } else {
            ESP_LOGE(TAG, "Invalid parameter");
        }

        err = esp_wifi_set_bandwidth(ifx, bandwidth);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Set bandwidth ERROR");
        } else {
            ESP_LOGI(TAG, "Set bandwidth SUCCESS");
        }
    } else {
        ifx = wifi_interface(bwd_args.get_interface->sval[0]);

        err = esp_wifi_get_bandwidth(ifx, &bandwidth);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Get bandwidth ERROR");
        } else {
            ESP_LOGI(TAG, "Current bandwidth is %s", (bandwidth == WIFI_BW_HT20) ? "HT20" : "HT40");
        }
    }

    return 0;

}

static int wifi_cmd_get_rate(const char * rate_str)
{
    char *rate_table[] = {
        "1ML",  "2ML", "5.5ML", "11ML", "RSVD", "2MS", "5.5MS", "11MS",
        "48M",  "24M", "12M",   "6M",   "54M",  "36M", "18M",   "9M",
        "MCS0L", "MCS1L", "MCS2L", "MCS3L", "MCS4L", "MCS5L", "MCS6L", "MCS7L",
        "MCS0S", "MCS1S", "MCS2S", "MCS3S", "MCS4S", "MCS5S", "MCS6S", "MCS7S"
    };

    if (!rate_str) {
        return WIFI_PHY_RATE_MAX;
    }

    for (int i=0; i<sizeof(rate_table)/sizeof(char*); i++) {
        if (!strcmp(rate_table[i], rate_str)) {
            return i;
        }
    }

    return WIFI_PHY_RATE_MAX;
}

static bool wifi_cmd_set_fix_rate(int ifx, const char* rate_str)
{
    int rate = wifi_cmd_get_rate(rate_str);

    if (rate == WIFI_PHY_RATE_MAX) {
        ESP_LOGI(TAG, "unknow rate");
        return false;
    }

    ESP_ERROR_CHECK( esp_wifi_internal_set_fix_rate(ifx, true, rate) );

    return true;
}

static int wifi_cmd_fix_rate(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &fix_rate_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, fix_rate_args.end, argv[0]);
        return 1;
    }

    ESP_LOGI(TAG, "set fix rate");
    if ( fix_rate_args.rate->count == 1 ) {
        wifi_cmd_set_fix_rate(WIFI_IF_STA, fix_rate_args.rate->sval[0]);
    } else {
        ESP_LOGI(TAG, "invalid arg number");
    }

    return 0;
}

static bool wifi_cmd_ap_set(const char* ssid, const char* pass)
{
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .max_connection = 4,
            .password = "",
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    reconnect = false;
    strlcpy((char*) wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    if (pass) {
        if (strlen(pass) != 0 && strlen(pass) < 8) {
            reconnect = true;
            ESP_LOGE(TAG, "password less than 8");
            return false;
        }
        strlcpy((char*) wifi_config.ap.password, pass, sizeof(wifi_config.ap.password));
    }

    if (strlen(pass) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    return true;
}

static int wifi_cmd_ap(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &ap_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, ap_args.end, argv[0]);
        return 1;
    }

    wifi_cmd_ap_set(ap_args.ssid->sval[0], ap_args.password->sval[0]);
    ESP_LOGI(TAG, "AP mode, %s %s", ap_args.ssid->sval[0], ap_args.password->sval[0]);
    return 0;
}

static int wifi_cmd_stats_show(const char *type)
{
    if (strcmp("hw", type) == 0) {
        dbg_cnt_lmac_hw_show();
    } else if (strcmp("int", type) == 0) {
        dbg_cnt_lmac_int_show();
    } else if (strcmp("lmac", type) == 0) {
        dbg_cnt_lmac_rxtx_show();
    } else if (strcmp("eb", type) == 0) {
        dbg_cnt_lmac_eb_show();
    } else if (strcmp("hmac", type) == 0) {
        dbg_cnt_hmac_rxtx_show();
    } else {
        ESP_LOGI(TAG, "unknow command type %s", type);
    }

    return 0;
}

static int wifi_cmd_stats(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &stats_args);

    if (nerrors != 0) {
        arg_print_errors(stderr, stats_args.end, argv[0]);
        return 1;
    }

    wifi_cmd_stats_show(stats_args.type->sval[0]);
    return 0;
}


static int wifi_cmd_query(int argc, char** argv)
{
    wifi_config_t cfg;
    wifi_mode_t mode;

    esp_wifi_get_mode(&mode);
    if (WIFI_MODE_AP == mode) {
        esp_wifi_get_config(WIFI_IF_AP, &cfg);
        ESP_LOGI(TAG, "AP mode, %s %s", cfg.ap.ssid, cfg.ap.password);
    } else if (WIFI_MODE_STA == mode) {
        int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);
        if (bits & CONNECTED_BIT) {
            esp_wifi_get_config(WIFI_IF_STA, &cfg);
            ESP_LOGI(TAG, "sta mode, connected %s", cfg.ap.ssid);
        } else {
            ESP_LOGI(TAG, "sta mode, disconnected");
        }
    } else {
        ESP_LOGI(TAG, "NULL mode");
        return 0;
    }

    return 0;
}

static uint32_t wifi_get_local_ip(void)
{
    int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);
    esp_netif_t * netif = netif_ap;
    esp_netif_ip_info_t ip_info;
    wifi_mode_t mode;

    esp_wifi_get_mode(&mode);
    if (WIFI_MODE_STA == mode) {
        bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, 0, 1, 0);
        if (bits & CONNECTED_BIT) {
            netif = netif_sta;
        } else {
            ESP_LOGE(TAG, "sta has no IP");
            return 0;
        }
     }

     esp_netif_get_ip_info(netif, &ip_info);
     return ip_info.ip.addr;
}

static int wifi_cmd_iperf(int argc, char** argv)
{
    int nerrors = arg_parse(argc, argv, (void**) &iperf_args);
    iperf_cfg_t cfg;

    if (nerrors != 0) {
        arg_print_errors(stderr, iperf_args.end, argv[0]);
        return 0;
    }

    memset(&cfg, 0, sizeof(cfg));

    if ( iperf_args.abort->count != 0) {
        iperf_stop();
        return 0;
    }

    if ( ((iperf_args.ip->count == 0) && (iperf_args.server->count == 0)) ||
         ((iperf_args.ip->count != 0) && (iperf_args.server->count != 0)) ) {
        ESP_LOGE(TAG, "should specific client/server mode");
        return 0;
    }

    if (iperf_args.ip->count == 0) {
        cfg.flag |= IPERF_FLAG_SERVER;
    } else {
        cfg.dip = esp_ip4addr_aton(iperf_args.ip->sval[0]);
        cfg.flag |= IPERF_FLAG_CLIENT;
    }

    cfg.sip = wifi_get_local_ip();
    if (cfg.sip == 0) {
        return 0;
    }

    if (iperf_args.udp->count == 0) {
        cfg.flag |= IPERF_FLAG_TCP;
    } else {
        cfg.flag |= IPERF_FLAG_UDP;
    }

    if (iperf_args.port->count == 0) {
        cfg.sport = IPERF_DEFAULT_PORT;
        cfg.dport = IPERF_DEFAULT_PORT;
    } else {
        if (cfg.flag & IPERF_FLAG_SERVER) {
            cfg.sport = iperf_args.port->ival[0];
            cfg.dport = IPERF_DEFAULT_PORT;
        } else {
            cfg.sport = IPERF_DEFAULT_PORT;
            cfg.dport = iperf_args.port->ival[0];
        }
    }

    if (iperf_args.interval->count == 0) {
        cfg.interval = IPERF_DEFAULT_INTERVAL;
    } else {
        cfg.interval = iperf_args.interval->ival[0];
        if (cfg.interval <= 0) {
            cfg.interval = IPERF_DEFAULT_INTERVAL;
        }
    }

    if (iperf_args.time->count == 0) {
        cfg.time = IPERF_DEFAULT_TIME;
    } else {
        cfg.time = iperf_args.time->ival[0];
        if (cfg.time <= cfg.interval) {
            cfg.time = cfg.interval;
        }
    }

    if (iperf_args.ip_tos->count != 0) {
        cfg.ip_tos = iperf_args.ip_tos->sval[0];
        cfg.flag |= IPERF_FLAG_IPTOS;
    }

    if (iperf_args.tcp_win_size->count != 0) {
        cfg.tcp_win_size = iperf_args.tcp_win_size->ival[0];
        if (cfg.tcp_win_size > IPERF_TCP_MAX_WIN_SIZE) {
            cfg.tcp_win_size = IPERF_TCP_MAX_WIN_SIZE;
        } else if (cfg.tcp_win_size < IPERF_TCP_MIN_WIN_SIZE) {
            cfg.tcp_win_size = IPERF_TCP_MIN_WIN_SIZE;
        }
        cfg.flag |= IPERF_FLAG_TCP_WIN;
    }

    ESP_LOGI(TAG, "mode=%s-%s sip=%d.%d.%d.%d:%d, dip=%d.%d.%d.%d:%d, interval=%d, time=%d",
            cfg.flag&IPERF_FLAG_TCP?"tcp":"udp",
            cfg.flag&IPERF_FLAG_SERVER?"server":"client",
            cfg.sip&0xFF, (cfg.sip>>8)&0xFF, (cfg.sip>>16)&0xFF, (cfg.sip>>24)&0xFF, cfg.sport,
            cfg.dip&0xFF, (cfg.dip>>8)&0xFF, (cfg.dip>>16)&0xFF, (cfg.dip>>24)&0xFF, cfg.dport,
            cfg.interval, cfg.time);

    iperf_start(&cfg);

    return 0;
}

void register_wifi(void)
{
    sta_args.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID of AP");
    sta_args.password = arg_str0(NULL, NULL, "<pass>", "password of AP");
    sta_args.end = arg_end(2);

    const esp_console_cmd_t sta_cmd = {
        .command = "sta",
        .help = "WiFi is station mode, join specified soft-AP",
        .hint = NULL,
        .func = &wifi_cmd_sta,
        .argtable = &sta_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&sta_cmd) );

    scan_args.ssid = arg_str0(NULL, NULL, "<ssid>", "SSID of AP want to be scanned");
    scan_args.end = arg_end(1);

    const esp_console_cmd_t scan_cmd = {
        .command = "scan",
        .help = "WiFi is station mode, start scan ap",
        .hint = NULL,
        .func = &wifi_cmd_scan,
        .argtable = &scan_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&scan_cmd) );
 
    reg_args.read_reg = arg_dbl0("r", "read", "<address>", "read address of register");
    reg_args.write_reg = arg_dbl0("w", "write", "<address>", "write address of register");
    reg_args.value = arg_dbl0("v", "value", "<value>", "value of register");
    reg_args.end = arg_end(1);

    const esp_console_cmd_t reg_cmd = {
	    .command = "reg",
	    .help = "Read/Write register",
	    .hint = NULL,
	    .func = &wifi_cmd_reg,
	    .argtable = &reg_args
    };
    
    ESP_ERROR_CHECK( esp_console_cmd_register(&reg_cmd) );
 
    tpw_args.get_max_tx_power = arg_lit0("g", "get", "get max tx power");
    tpw_args.set_max_tx_power = arg_int0("s", "set", "<value>", "set max tx power");
    tpw_args.end = arg_end(1);

    const esp_console_cmd_t tpw_cmd = {
	    .command = "tpw",
	    .help = "Get/Set max tx power, unit is 0.25dBm",
	    .hint = NULL,
	    .func = &wifi_cmd_tpw,
	    .argtable = &tpw_args
    };
   
    ESP_ERROR_CHECK( esp_console_cmd_register(&tpw_cmd) );

    pro_args.get_interface = arg_str0("g", "get", "<sta/ap/eth>", "Get the current protocol bitmap of the specified interface");
    pro_args.set_interface = arg_str0("s", "set", "<sta/ap/eth>", "Set interface");
    pro_args.protocol = arg_str0("p", "protocol", "<bgn/gn/n>", "Set the value of the specified interface protocol type");
    pro_args.end = arg_end(1);

    const esp_console_cmd_t pro_cmd = {
          .command = "pro",
          .help = "Get/Set protocol type of specified interface", 
          .hint = NULL,
          .func = &wifi_cmd_pro,
          .argtable = &pro_args
      };

    ESP_ERROR_CHECK( esp_console_cmd_register(&pro_cmd) );

    bwd_args.get_interface = arg_str0("g", "get", "<sta/ap/eth>", "Get the bandwidth of ESP32 specified interface");
    bwd_args.set_interface = arg_str0("s", "set", "<sta/ap/eth>", "Set ESP32 specified interface");
    bwd_args.bandwidth = arg_str0("b", "bandwidth", "<ht20/ht40>", "Set the bandwidth value of the specified interface of ESP32");
    bwd_args.end = arg_end(1);

    const esp_console_cmd_t bwd_cmd = {
        .command = "bwd",
        .help = "Get/Set the bandwidth of ESP32 specified interface",
        .hint = NULL,
        .func = &wifi_cmd_bwd,
        .argtable = &bwd_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&bwd_cmd) );

    fix_rate_args.rate = arg_str0(NULL, NULL, "<rate_str>", "rate such as 1ML, 5.5MS, MCS0L, MCS7S etc, L-Long, S-Short");
    fix_rate_args.end = arg_end(1);

    const esp_console_cmd_t fix_rate_cmd = {
        .command = "fix_rate",
        .help = "Set fix rate",
        .hint = NULL,
        .func = &wifi_cmd_fix_rate,
        .argtable = &fix_rate_args
    };
    
    ESP_ERROR_CHECK( esp_console_cmd_register(&fix_rate_cmd) );

    ap_args.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID of AP");
    ap_args.password = arg_str0(NULL, NULL, "<pass>", "password of AP");
    ap_args.end = arg_end(2);
    
    const esp_console_cmd_t ap_cmd = {
        .command = "ap",
        .help = "AP mode, configure ssid and password",
        .hint = NULL,
        .func = &wifi_cmd_ap,
        .argtable = &ap_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&ap_cmd) );

    const esp_console_cmd_t query_cmd = {
        .command = "query",
        .help = "query WiFi info",
        .hint = NULL,
        .func = &wifi_cmd_query,
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&query_cmd) );

    stats_args.type = arg_str1(NULL, NULL, "<hw|int|lmac|hmac>", "show statistics");
    stats_args.end = arg_end(2);
    const esp_console_cmd_t stats_cmd = {
        .command = "stats",
        .help = "query WiFi statistics",
        .hint = NULL,
        .func = &wifi_cmd_stats,
        .argtable = &stats_args
    };
    ESP_ERROR_CHECK( esp_console_cmd_register(&stats_cmd) );

    iperf_args.ip = arg_str0("c", "client", "<ip>", "run in client mode, connecting to <host>");
    iperf_args.server = arg_lit0("s", "server", "run in server mode");
    iperf_args.udp = arg_lit0("u", "udp", "use UDP rather than TCP");
    iperf_args.port = arg_int0("p", "port", "<port>", "server port to listen on/connect to");
    iperf_args.interval = arg_int0("i", "interval", "<interval>", "seconds between periodic bandwidth reports");
    iperf_args.time = arg_int0("t", "time", "<time>", "time in seconds to transmit for (default 10 secs)");
    iperf_args.ip_tos = arg_str0("S", "tos", "<precedence TID0~TID7>", "set IP TOS");
    iperf_args.tcp_win_size = arg_int0("w", "window", "<window size>", "set TCP window size (socket buffer size)");
    iperf_args.abort = arg_lit0("a", "abort", "abort running iperf");
    iperf_args.end = arg_end(1);
    const esp_console_cmd_t iperf_cmd = {
        .command = "iperf",
        .help = "iperf command",
        .hint = NULL,
        .func = &wifi_cmd_iperf,
        .argtable = &iperf_args
    };

    ESP_ERROR_CHECK( esp_console_cmd_register(&iperf_cmd) );
}
