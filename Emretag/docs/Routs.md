Dört yol
1. Açılış


SYS_INIT(imu_power_on)  POST_KERNEL 60   [imu.c]   ← main'den önce
K_THREAD_DEFINE(imu_thread)              [imu.c]
main()                                   [main.c]
 ├─ outputs_init()                       [outputs.c]
 ├─ ble_cs_start()                       [ble_cs.c]
 │   ├─ select_ceramic_antenna()
 │   ├─ bt_enable()
 │   └─ bt_le_adv_start()
 ├─ app_gatt_init()                      [app_gatt.c]
 │   ├─ button_init()                    [button.c]
 │   └─ imu_motion_detect_enable()       [imu.c]
 └─ ble_cs_run()   → sem_connected'da bloke
2. Butona basılması


button_isr()  ← ISR              [button.c]
 └─ k_work_submit()
     └─ button_work_handler()
         └─ on_button_change()   [app_gatt.c]
             ├─ app_gatt_notify_button() → notify_value() → bt_gatt_notify()
             └─ ble_cs_set_press_count() → bt_le_adv_update_data()   [ble_cs.c]
3. Çift tap


imu_int_isr()  ← ISR             [imu.c]
 └─ k_work_submit()
     └─ imu_int_work_handler()
         ├─ i2c: WAKE_UP_SRC, TAP_SRC oku
         └─ report() → on_imu_event()    [app_gatt.c]
             └─ handle_double_tap()
4. Telefonun buzzer'ı açması


BT stack → write_output()        [app_gatt.c]
            └─ outputs_set()     [outputs.c]
                └─ gpio_pin_set_dt()