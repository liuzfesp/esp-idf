#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Register WiFi functions
void register_wifi(void);
void initialise_wifi(void);
void dbg_cnt_lmac_eb_show(void);                                                                                                  
void dbg_cnt_lmac_hw_show(void);
void dbg_cnt_lmac_int_show(void);
void dbg_cnt_lmac_rxtx_show(void);
void dbg_cnt_hmac_rxtx_show(void);

#ifdef __cplusplus
}
#endif
