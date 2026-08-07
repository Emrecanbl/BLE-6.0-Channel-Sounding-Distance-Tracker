# EmreTag — Teknik Mülakat Notları

Bu kod tabanı üzerinden gelebilecek her soruya cevap verebilmek için hazırlanmış
referans. Teknik terimler kodda göründükleri gibi İngilizce bırakıldı.

---

## 0. Otuz saniyelik özet

> nRF54L15 üzerinde çalışan, Bluetooth 6.0 Channel Sounding ile iki cihaz
> arasındaki **gerçek mesafeyi** ölçen batarya beslemeli bir tracker. Tag
> reflector rolünde, nRF54L15 DK initiator. Mesafe verisi standart **Ranging
> Service (RAS)** üzerinden gidiyor. Bunun yanında kendi GATT profilim var:
> buton durumu, LED/buzzer/titreşim kontrolü, ve iki olay karakteristiği —
> "telefonumu bul" ve "düşme algılandı". IMU'nun kendi hareket/tap/serbest düşüş
> motorlarını register seviyesinde yapılandırıp tek bir kesme hattından
> okuyorum. Zephyr RTOS, nRF Connect SDK, sysbuild.

**Neden RSSI değil:** RSSI sinyal gücüdür; engel, vücut gölgelemesi ve
yönelimden etkilenir. "Zayıf sinyal" ile "uzak" aynı şey değildir. Channel
Sounding faz ve zamanlamadan mesafe türetir, metre verir.

---

## 1. Mimari

```
main.c        → sadece açılış sırası ve modüllerin birbirine bağlanması
ble_cs.c/h    → bt_enable, advertising, bağlantı callback'leri, CS reflector
app_gatt.c/h  → kendi GATT profilim
imu.c/h       → besleme, polling thread, filtre, on-chip algılama motorları
button.c/h    → sw0 GPIO + kesme + work queue devri
outputs.c/h   → LED / buzzer / titreşim, darbe ve tekrarlayan desen
```

**Bağımlılık yönü tek taraflı.** Modüller birbirini tanımıyor, `main.c`
bağlıyor. Örnek: buton değiştiğinde `button.c` sadece kayıtlı callback'i
çağırıyor; o callback `main.c`'de ve içinden `app_gatt_notify_button()` ile
`ble_cs_set_press_count()` çağrılıyor. `button.c`'nin BLE'den haberi yok.

**Neden böyle:** her modül tek başına test edilebilir ve `outputs.c`'yi
değiştirirken BLE koduna dokunmam gerekmiyor. Ayrıca `imu.h`'deki
`imu_get_latest()` ileride Edge Impulse tabanlı bir jest sınıflandırıcının
bağlanacağı yer — o modül sensör sürücüsüyle değil bu arayüzle konuşacak.

> **Dürüst not:** başlangıçta hepsi tek dosyaydı, 678 satıra çıkınca böldüm.
> "Ne zaman bölersin?" diye sorulursa: erken bölmek erken soyutlamadır,
> sınırların nereye düşeceğini bilmeden bölme.

---

## 2. Açılış sırası — bu bölüm çok soru gelir

Zephyr'de init seviyeleri: `EARLY` → `PRE_KERNEL_1` → `PRE_KERNEL_2` →
`POST_KERNEL` → `APPLICATION`, her seviyede 0-99 arası öncelik.

Bu projedeki sıra:

| Öncelik | Ne | Neden önemli |
|---|---|---|
| 41 | `GPIO_HOGS_INIT_PRIORITY` | IMU besleme pinini sürüyor |
| 45 | `REGULATOR_FIXED_INIT_PRIORITY` | `pdm_imu_pwr` açılıyor, DTS'teki `startup-delay-us = <5000>` uygulanıyor |
| 50 | `I2C_INIT_PRIORITY` | i2c30 hazır |
| **60** | **`SYS_INIT(imu_power_on, POST_KERNEL, 60)`** | **benim kodum** |
| 90 | `SENSOR_INIT_PRIORITY` | LSM6DSL sürücüsü WHO_AM_I okuyor |
| — | `main()` | APPLICATION seviyesinden sonra |

**Soru: "IMU beslemesini neden `main()` içinde açmadın?"**

Çünkü çok geç olurdu. Sensör sürücüsünün init'i POST_KERNEL 90'da çalışıp I²C
üzerinden WHO_AM_I okuyor. `main()` bundan sonra çalışır. Besleme yoksa init
başarısız olur ve `device_is_ready()` **kalıcı olarak** false döner — sonradan
beslemeyi açmak bunu değiştirmez, çünkü init tekrar çalışmaz.

Bu tam olarak yaşadığım hataydı: "IMU not ready" alıyordum ve ilk içgüdüm
gecikme eklemekti. Gecikme çözmez; init sırası çözer.

**Soru: "`SYS_INIT` ile `main()` arasındaki fark?"**
`SYS_INIT` çekirdek başlatma zincirinin bir parçası, önceliğe göre sıralanır ve
`main()`'den önce çalışır. Sürücüler arası bağımlılıkları çözmek için doğru yer.

**Soru: "POST_KERNEL'de `k_msleep` çağırabilir misin?"**
Evet. POST_KERNEL ve APPLICATION seviyeleri main thread bağlamında çalışır,
çekirdek ayaktadır. PRE_KERNEL seviyelerinde uyuyamazsın, orada `k_busy_wait`
kullanılır.

---

## 3. Devicetree

**`DT_ALIAS` vs `DT_NODELABEL`:**
- `DT_ALIAS(sw0)` → board'un `aliases` bloğundaki standart isim. Taşınabilir:
  hangi board olursa olsun `sw0` kullanıcı butonudur.
- `DT_NODELABEL(gpio1)` → node'un etiketiyle doğrudan erişim. Board'a özgü.

Kodda buton ve LED alias ile (`sw0`, `led0`), buzzer ve titreşim motoru ise
node label + pin numarasıyla tanımlı — çünkü board DTS'inde onlar için node yok
ve iki pin için overlay dosyası açmaya değmedi.

**`gpio_dt_spec` nedir:** port device pointer'ı, pin numarası ve DT flag'lerini
tutan yapı. Önemli olan `dt_flags` içindeki `GPIO_ACTIVE_LOW`: `gpio_pin_set_dt(&spec, 1)`
**mantıksal** "aç" demek, sürücü aktif-düşük donanımı kendi çeviriyor. Kodda
hiçbir yerde ters çevirme yok.

```c
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(SW0_NODE, gpios);
/* board DTS'inde yoksa elle: */
.spec = { .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)), .pin = 4,
          .dt_flags = GPIO_ACTIVE_HIGH },
```

**`DEVICE_DT_GET` derleme zamanında çözülür**, çalışma zamanında arama yapmaz.
`device_is_ready()` ile init'in başarılı olup olmadığını kontrol etmek gerekir.

**Overlay tuzağı:** `boards/` altına **yeni** bir overlay dosyası eklersen CMake
onu artımlı derlemede görmez, `-p always` (pristine) gerekir. Mevcut bir
overlay'i düzenlersen fark eder. Bunu yaşadım: `DT_N_ALIAS_buzzer... undeclared`
hatası tam olarak buydu.

---

## 4. Kconfig ve sysbuild

**Kconfig** derleme zamanı özellik seçimi. `prj.conf` uygulamanın, `sysbuild.conf`
ise **çoklu imaj** yapılandırmasının (MCUboot + uygulama) ayarları.

**sysbuild nedir:** birden fazla imajı (bootloader, uygulama, network core) tek
build'de yöneten üst seviye sistem. Bu projede `SB_CONFIG_BOOTLOADER_MCUBOOT=y`
MCUboot'u ayrı bir imaj olarak derletiyor.

**Bu projedeki kritik Kconfig kararları:**

| Ayar | Neden |
|---|---|
| `CONFIG_LSM6DSL_TRIGGER_NONE=y` | Sürücünün `irq-gpios` pinini sahiplenmemesi için — INT1'i ben düz GPIO olarak kullanıyorum |
| `CONFIG_LSM6DSL_ACCEL_ODR=4` | Varsayılan **0** ve bu "runtime'da seçilir" demek, yani sensör power-down kalır ve hep sıfır okursun. Klasik tuzak. |
| `CONFIG_CBPRINTF_FP_SUPPORT=y` | Log'da `%f` kullanabilmek için |
| `CONFIG_BT_L2CAP_TX_MTU=498` | Ranging Profile en az 247 oktet MTU öneriyor |
| `CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW=y` | `CONFIG_BT_SMP=y` varken SMP servisi varsayılan olarak kimlik doğrulama ister; `BT_BONDABLE=n` olduğu için DFU ATT hatasıyla düşerdi |

---

## 5. Eşzamanlılık — en çok soru gelen konu

**Altın kural: ISR içinde bloke olabilecek hiçbir şey çağrılmaz.**

Kodda üç yerde bu kural işliyor:

1. **Buton ISR'ı** (`button.c`) — sadece pini okuyor, sayacı artırıyor ve
   `k_work_submit()` yapıyor. `bt_gatt_notify()` ve `bt_le_adv_update_data()`
   HCI komutu gönderip bloke olabildiği için work queue'da çalışıyor.

2. **IMU INT1 ISR'ı** (`imu.c`) — kaynak register'ları okumak I²C gerektiriyor,
   I²C ISR'da yapılamaz. ISR sadece `k_work_submit()`.

3. **GATT yazma callback'i** (`app_gatt.c`) — sıfırlama sonrası bildirim
   doğrudan gönderilmiyor, `k_work_submit()` ile erteleniyor. Böylece önce
   write response çıkıyor, bildirim sonra.

**`k_work` vs `k_work_delayable` vs `k_timer`:**
- `k_work` — hemen kuyruğa girer, thread bağlamında çalışır
- `k_work_delayable` — gecikmeli, yine thread bağlamında
- `k_timer` — callback'i **ISR bağlamında** çalışır, orada bloke olamazsın

`outputs.c`'de `k_work_delayable` kullandım çünkü GPIO yazması yeterince basit
olsa da aynı mekanizmayı ileride PWM ve daha karmaşık desenler için
kullanacağım; thread bağlamı orada gerekli olacak.

**`k_work_submit` tuzağı:** iş zaten kuyruktayken tekrar submit edilirse ikinci
kez kuyruğa girmez. Bu yüzden buton sayacını ISR'da artırıyorum — work
handler'da artırsaydım hızlı basışlar birleşir ve sayaç eksik kalırdı.

**`k_work_reschedule` vs `k_work_schedule`:** `reschedule` zamanlayıcıyı
sıfırdan başlatır, `schedule` zaten planlanmışsa dokunmaz. `outputs_pulse()`
içinde `reschedule` kullanıyorum ki üst üste gelen darbeler süreyi uzatsın,
erken kesmesin.

**`outputs_blink_stop()` sırası önemli:** önce `k_work_cancel_delayable()`,
sonra pini indir. Ters sırada yapsan yoldaki bir tick çıkışı tekrar açabilir.

**Mutex:** `imu.c`'de `latest` örneği polling thread'i yazıyor,
`imu_get_latest()` başka bir thread'den okuyabiliyor — arada `k_mutex`.

---

## 6. Modül modül detay

### 6.1 `outputs.c`

```c
struct gpio_output {
    const struct gpio_dt_spec spec;
    const char *name;
    uint8_t state;
    struct k_work_delayable tick_work;
    uint32_t on_ms, off_ms;
    bool repeating;
};
```

Üç çıkış (LED, buzzer, motor) aynı yapıyı paylaşıyor. GATT tarafında
`attr->user_data` bu yapıyı gösteriyor, böylece **tek bir** `read_output` /
`write_output` çifti üç karakteristiğe birden hizmet ediyor.

`output_tick_work_handler()` içinde `CONTAINER_OF` ile work item'dan sahip
yapıya geri dönülüyor — Zephyr'de yaygın kalıp:

```c
struct k_work_delayable *dwork = k_work_delayable_from_work(work);
struct gpio_output *out = CONTAINER_OF(dwork, struct gpio_output, tick_work);
```

`repeating` bayrağı tek atımlık darbe ile tekrarlayan deseni ayırıyor.

### 6.2 `button.c`

`GPIO_INT_EDGE_BOTH` kullanılıyor — sadece basma kenarını dinleseydim bırakma
anını göremez, GATT'taki durum 1'de takılı kalırdı.

`gpio_pin_get_dt()` mantıksal seviye döndürüyor, DTS'teki `GPIO_ACTIVE_LOW`
otomatik hesaba katılıyor.

`button_init()` **advertising başladıktan sonra** çağrılıyor (`main.c`), çünkü
erken bir basış `bt_le_adv_update_data()`'ya BLE stack hazır değilken girerdi.

### 6.3 `imu.c`

**İki ayrı mekanizma var, karıştırma:**

**(a) Polling thread** — `sensor_sample_fetch()` + `sensor_channel_get()` ile
104 Hz'de örnekleme, EMA alçak geçiren filtre, `imu_get_latest()` ile dışarı
açılıyor. Bu Zephyr'in standart sensor API'si.

```c
*state = first ? raw : (ALPHA * raw) + ((1.0 - ALPHA) * *state);
```

**(b) On-chip algılama motorları** — Zephyr'in `lsm6dsl` sürücüsü **sadece**
`SENSOR_TRIG_DATA_READY` destekliyor, başka trigger tipinde assert atıyor. Yani
wake-up, tap, free-fall ve inactivity motorlarına sensor API'sinden erişilemiyor.
Bu yüzden çipin register'larına aynı I²C hattından **doğrudan** yazıyorum ve
INT1'i düz GPIO kesmesi olarak alıyorum.

**Register haritası (LSM6DS3TR-C, LSM6DSL ile uyumlu):**

| Register | Adres | Kullanılan alanlar |
|---|---|---|
| `WAKE_UP_SRC` | 0x1B | FF_IA (b5), SLEEP_STATE_IA (b4), WU_IA (b3) |
| `TAP_SRC` | 0x1C | SINGLE_TAP (b5), DOUBLE_TAP (b4) |
| `TAP_CFG` | 0x58 | INTERRUPTS_ENABLE (b7), INACT_EN (b6:5), SLOPE_FDS (b4), TAP_X/Y/Z_EN (b3:1), LIR (b0) |
| `TAP_THS_6D` | 0x59 | TAP_THS (b4:0) |
| `INT_DUR2` | 0x5A | DUR (b7:4), QUIET (b3:2), SHOCK (b1:0) |
| `WAKE_UP_THS` | 0x5B | SINGLE_DOUBLE_TAP (b7), WK_THS (b5:0) |
| `WAKE_UP_DUR` | 0x5C | FF_DUR5 (b7), WAKE_DUR (b6:5), SLEEP_DUR (b3:0) |
| `FREE_FALL` | 0x5D | FF_DUR (b7:3), FF_THS (b2:0) |
| `MD1_CFG` | 0x5E | INT1_INACT_STATE (b7), INT1_SINGLE_TAP (b6), INT1_WU (b5), INT1_FF (b4), INT1_DOUBLE_TAP (b3) |

**Dört tane "bunu bilmeden yapamazsın" detayı — mülakatta anlatılacak cinsten:**

1. **`SLOPE_FDS` biti şart.** Yüksek geçiren filtreyi devreye alıyor. Olmazsa
   sabit duran cihazın üzerindeki 1 g yerçekimi eşiğe dahil olur ve wake-up
   sürekli tetiklenir.

2. **Sadece çift tap istiyorsan `SINGLE_DOUBLE_TAP` bitini kapatma.** O bit
   kapalıyken çip sadece tek tap üretir. Doğru yol: biti açık bırak, tek tapı
   `MD1_CFG`'de INT1'e **yönlendirme**. Ayrıca yazılımda da filtrele, çünkü bir
   çift tap her zaman tek tap bitini de set eder.

3. **`FF_DUR` iki register'a bölünmüş.** Alt beş bit `FREE_FALL` 0x5D'de b7:3,
   altıncı bit `WAKE_UP_DUR` 0x5C'nin b7'sinde. Tek tabloya bakıp çevirirsen
   kaçırırsın.

4. **`LIR` (latched interrupt)** açık: kesme, kaynak register okunana kadar
   mandallı kalır. Yani work handler'ın `WAKE_UP_SRC`/`TAP_SRC` okuması sadece
   olayı öğrenmek için değil, **pini serbest bırakmak** için de gerekli.

**Hassasiyet ayarı mantığı:**
- `wake_threshold`: ±2 g'de 1 adım ≈ 31 mg. 8 ≈ 250 mg (eline alma), 32 ≈ 1 g
  (belirgin sallama).
- `wake_duration`: kaç ardışık örnek eşiğin üstünde kalmalı (0-3). **Tek
  darbeleri elemekte eşikten çok daha etkili** — masaya vuruş bir örnekte biter,
  taşımak sürer.
- Free-fall: eşik **yükseldikçe** ve süre **kısaldıkça** algılama artar.

**Inactivity motoru** (`INACT_EN`): hareketsizlik sürünce çip **kendi**
örnekleme hızını 12,5 Hz'e düşürüyor, hareket dönünce kendi geri açıyor. MCU
hiç karışmıyor. Bu, güç tasarımının sensör tarafı.

### 6.4 `app_gatt.c`

**Servis yapısı** — Nordic LED Button Service UUID tabanı üzerine kurulu, ilk üç
karakteristik jenerik istemcilerce tanınıyor:

| UUID soneki | İsim | Erişim |
|---|---|---|
| `1523` | (servis) | — |
| `1524` | Button | read, notify |
| `1525` | LED | read, write |
| `1526` | Buzzer | read, write |
| `1527` | Vibration motor | read, write |
| `1528` | Find phone | read, write, notify |
| `1529` | Fall detected | read, write, notify |

**CCC (Client Characteristic Configuration, 0x2902):** istemcinin bildirimleri
açıp kapattığı descriptor. `BT_GATT_CCC()` makrosu ekliyor, callback'te
`value == BT_GATT_CCC_NOTIFY` kontrolüyle bayrak tutuyorum.

**CUD (Characteristic User Description, 0x2901):** karakteristiğe insan
tarafından okunabilir isim veren descriptor. Bunu eklemezsen nRF Connect
"Unknown Characteristic" gösterir. Nordic'in `1523/1524/1525` UUID'lerini
uygulama kendi sözlüğünden tanıyor, benim eklediğim `1526+` için CUD şart.

**Notify vs Indicate:** notify onaysız, indicate onaylı. Buton durumu ve olaylar
için notify yeterli.

**Attribute indeksleme — dikkat edilecek nokta:**
```c
bt_gatt_notify(NULL, &app_svc.attrs[2], ...);   /* buton value attribute */
```
`BT_GATT_SERVICE_DEFINE` bir attribute dizisi üretiyor: [0] primary service,
[1] characteristic declaration, [2] characteristic value, [3] CCC, [4] CUD...
Araya karakteristik eklersen indeksler kayar. Yeni eklediğim find-phone ve fall
için bu yüzden **UUID ile arama** kullanıyorum:
```c
attr = bt_gatt_find_by_uuid(app_svc.attrs, app_svc.attr_count, BT_UUID_FALL_CHRC);
```
Nadiren çağrıldığı için doğrusal aramanın maliyeti önemsiz, karşılığında kırılgan
indeks aritmetiğinden kurtuluyorum.

**Olay karakteristiklerinde bayrak değil sayaç:** telefon bağlı değilken ya da
bildirimi kaçırdığında, tekrar bağlanıp okuduğunda sayacın arttığını görüyor.
Salt `1` gönderseydim o bilgi kaybolurdu. Telefon `0` yazarak onaylıyor.
Sıfır dışı değerler reddediliyor (`BT_ATT_ERR_VALUE_NOT_ALLOWED`) — sayacı
sadece cihaz artırabilsin diye.

### 6.5 `ble_cs.c`

**Advertising boyut hesabı — mutlaka bil, klasik soru:**

Legacy advertising payload'u **31 bayt**. Her AD elemanı 2 bayt header (length +
type) + veri.

```
ad[]:  flags 1+2=3 | UUID16 2+2=4 | isim 7+2=9 | mfg data 4+2=6  = 22/31
sd[]:  URI 23+2=25                                                = 25/31
```

Bir ara scan response'a 128-bit UUID eklemiştim: 16+2=18, toplam 43 > 31 →
`bt_le_adv_start()` **-EINVAL** döndü. Aşarsan sessizce kesilmez, hata alırsın.

**Manufacturer data:** ilk iki bayt Company Identifier (0x0059 = Nordic),
sonrası serbest. Ben buton basış sayacını koyuyorum. Bağlanmadan okunabilir
olması avantaj.

**Channel Sounding akışı (reflector tarafı):**
1. Bağlantı kurulur → `sem_connected`
2. `bt_le_cs_set_default_settings()` — reflector rolü açık, initiator kapalı
3. Initiator config oluşturur → `le_cs_config_complete` callback → `sem_config`
4. `bt_le_cs_set_procedure_parameters()` — subevent uzunluğu, PHY, anten yapılandırması
5. Initiator security enable ve procedure enable yapar, biz callback'lerle takip ederiz

**RAS (Ranging Service):** mesafe verisi benim profilimden değil, Bluetooth
SIG'in standart Ranging Service'inden gidiyor. NCS'in `bt_ras` kütüphanesi
sağlıyor. `CONFIG_BT_RAS_RRSP_AUTO_ALLOC_INSTANCE=y` olduğu için kütüphane
bağlantı callback'inde kendi `bt_ras_rrsp_alloc()` çağrısını yapıyor —
uygulamanın çağırmasına gerek yok.

> Bu detayı bilmemek beni bir kere yanılttı: kodda çağrı göremeyince "RAS bağlı
> değil" sonucuna varmıştım, oysa kütüphane kendi hallediyordu.

**`sys_reboot()` on disconnect:** upstream örnekten miras. CS durumunu elle
temizlemek yerine soğuk reset atıyor. Bilinen bir zayıflık, düşük güç
tasarımıyla çelişiyor, kaldırılması gerekiyor. **Mülakatta bunu kendin söyle** —
kodu okuyan zaten görecek.

---

## 7. Uygulama mantığı — jestler

**Find phone (iki aşamalı onay):**
```
çift tap ──2 sn içinde── çift tap  →  1 sn titreşim + "armed"
                                       │
                          5 sn içinde aynı jest tekrar
                                       ↓
                              GATT find-phone bildirimi
```
Neden iki aşama: masaya bir vuruş tek jesti üretmeye yeter, yanlış tetikleme
olur. İkinci onay bunu pratikte imkânsız kılıyor.

Zamanlama tamamen `k_uptime_get()` karşılaştırmasıyla — hiçbir yerde bekleme
yok, work queue tutulmuyor. Süre dolması için zamanlayıcı da yok: geç gelen jest
yeni bir tur başlatıyor.

**Düşme alarmı:**
```
1 sn içinde 4'ten fazla free-fall olayı  →  alarm
                                             ├─ buzzer 500ms açık / 500ms kapalı
                                             ├─ motor  aynı desen
                                             └─ GATT fall bildirimi
alarm durur: telefondan 0 yazılınca veya çift-çift tap jestiyle
```
Tek free-fall olayı çok kolay üretiliyor, bu yüzden patlama (burst) sayılıyor.

**Öncelik kuralı:** alarm çalarken jest find-phone tetiklemiyor, **alarmı
susturuyor**. Alarm çalan cihaza vuran kullanıcının kastettiği şey budur.

---

## 8. MCUboot / DFU

**Zincir:** derleme sırasında imaj **özel** anahtarla imzalanır, karşılık gelen
**açık** anahtar MCUboot binary'sinin içine gömülür. Açılışta MCUboot imzayı
doğrular; tutmazsa imajı çalıştırmaz.

**Neden `SWAP_USING_MOVE`:** eski imaj diğer slotta korunur, dolayısıyla geri
dönüş mümkün. `OVERWRITE_ONLY` eski imajı siler, geri dönecek bir şey kalmaz.

**Rollback mantığı:** yeni imaj "test" olarak işaretlenip yüklenir. Açılır ama
uygulama kendini `boot_write_img_confirmed()` ile onaylamazsa, sonraki resette
MCUboot eski imaja döner. Yani "açıldı" ile "çalışıyor" ayrımı yapılabiliyor.

**Bölüm düzeni (nRF54L15, devicetree'den — Partition Manager değil):**
```
boot_partition   62 KB   MCUboot
slot0_partition 664 KB   çalışan imaj
slot1_partition 664 KB   indirilen imaj
storage_partition 36 KB  settings
```

**SMP / MCUmgr:** DFU taşıması. `CONFIG_MCUMGR_TRANSPORT_BT=y` ile BLE üzerinden
SMP servisi açılıyor, nRF Device Manager uygulaması yüklüyor.

**Yakaladığım tuzak:** `CONFIG_BT_SMP=y` varken SMP servisinin varsayılan izni
`RW_AUTHEN` — şifreleme *ve* kimlik doğrulama ister. Bu projede
`CONFIG_BT_BONDABLE=n`, yani eşleşme yok. Varsayılanda bırakılsa her DFU denemesi
ATT authentication hatasıyla düşerdi ve sebebi hiç görünmezdi. `PERM_RW`'ye
çektim; ürünleşirken bonding ekleyip geri almak gerekir.

---

## 9. İki savaş hikâyesi — bunları mutlaka anlat

### 9.1 RF anahtarı

XIAO nRF54L15 radyoyu bir RF switch üzerinden geçiriyor: dahili seramik anten mi,
harici u.FL mi. Board devicetree'sindeki `rfsw_pwr` ve `rfsw_ctl` node'ları
boot'ta etkinleştirilmiyor ve upstream Channel Sounding örnekleri bu anahtardan
habersiz. Sonuç: RF yolu tanımsız kalıyor, link bütçesi çöküyor.

Anahtarı besleyip seramik anteni açıkça seçince **kullanılabilir menzil ~2-3
m'den >12 m'ye çıktı.** `bt_enable()`'dan önce yapılması gerekiyor.

**Neden bu iyi bir hikâye:** SDK örneğinin donanım detayını atladığını fark
etmek, düzeltmek ve **sonucu ölçmek**. Tutorial takip etmekle mühendislik
arasındaki fark.

### 9.2 MPU fault

**Belirti:** buton GPIO kesmesi açıldığında MPU fault, her zaman aynı komutta
(`zephyr/lib/net_buf/buf.c:82`, `pool_get_uninit()` içinde `buf->pool_id`
yazarken), ama fault adresi build'den build'e değişiyor (0x9, 0x33259, 0x1a).

**Teşhis yöntemi:**
1. Fault dump'taki PC'yi `arm-zephyr-eabi-addr2line -e zephyr.elf 0x1d792` ile
   kaynak satırına çevirdim
2. `objdump` ile `net_buf_alloc_len`'i çağıran yerleri buldum
3. `zephyr.map`'ten havuzun RAM adresini ve komşularını çıkardım
4. Havuz işaretçisinin kendisi geçerliydi ama `__bufs` alanı çöp okunuyordu →
   yapının **içeriği** bozuluyor, yani bellek bozulması

**Kritik gözlem:** `LOG_INF` eklenince semptom kayboldu. Bu **düzeltme değil**;
binary yerleşimi değişti, çöp değer zararsız bir yere düştü. Modüllere ayırma
yerleşimi tekrar değiştirince hata geri geldi.

**İkinci kanıt:** bir yakalamada çip LOCKUP durumundaydı (`pc: 0xfffffffe`),
OpenOCD "clearing lockup after double fault" dedi. Double fault, fault
handler'ın kendisinin fault vermesi demek — genelde exception giriş anında
stack frame yazılamaması, yani **stack bozuk**.

**Ana şüphelim:** sistem work queue'sunda stack overflow. Varsayılan boyut 1024
bayt ve orada hem `bt_gatt_notify()` hem `bt_le_adv_update_data()` çağrılıyor,
ikisi de derin BT host çağrı zincirleri.

**Sıradaki adım (henüz yapılmadı):** `CONFIG_THREAD_ANALYZER` ile thread stack
kullanımını ölçmek. Backtrace için Cortex-M'de `CONFIG_EXTRA_EXCEPTION_INFO=y`
gerekiyor — `ARCH_HAS_STACKWALK` ona bağlı.

**Mülakatta:** bu hatayı çözülmüş gibi anlatma. "Semptom kayboldu ama sebep
duruyor, ölçüm planım şu" demek çok daha güçlü — çünkü heisenbug'ı tanıdığını ve
'çalışıyor gibi görünmek' ile 'düzeltilmiş olmak' arasındaki farkı bildiğini
gösterir.

---

## 10. Muhtemel sorular ve cevaplar

**"Bu kodda ISR'da ne yapıyorsun?"**
Mümkün olan en az şeyi: pini okumak, sayaç artırmak, work submit etmek. Bloke
olabilecek her şey work queue'da.

**"`k_work_submit` başarısız olabilir mi?"**
Negatif dönebilir. Ayrıca iş zaten kuyruktaysa tekrar kuyruğa girmez — bu yüzden
sayacı ISR'da artırıyorum.

**"Neden `k_timer` değil `k_work_delayable`?"**
`k_timer` callback'i ISR bağlamında çalışır. Çıkışları kapatmak şu an basit ama
aynı mekanizmayı PWM desenleri için kullanacağım, orada thread bağlamı gerekli.

**"Bu kodu nRF52'ye taşısan ne değişir?"**
Devicetree ve board dosyaları. Uygulama kodu `DT_ALIAS` kullandığı için büyük
ölçüde aynı kalır. Channel Sounding nRF52'de yok (Bluetooth 6.0 gerektiriyor),
o kısım düşer. Buzzer/motor pin tanımları board'a göre değişir.

**"Advertising 31 baytı aşarsa ne olur?"**
`bt_le_adv_start()` `-EINVAL` döner. Sessizce kesilmez.

**"Extended advertising ne zaman gerekir?"**
31 bayt yetmediğinde. Bluetooth 5 ile geldi, `bt_le_ext_adv_*` API'si kullanılır.
Bu projede gerek olmadı, veriyi kırptım.

**"GATT'ta notify ile indicate farkı?"**
Notify onaysız ve hızlı; indicate istemciden onay bekler. Ben notify kullanıyorum.

**"Bu profili neden standart bir servis yerine kendin yazdın?"**
Mesafe için zaten standardı kullanıyorum (RAS). Buton/LED/buzzer/titreşim ve
uygulamaya özgü iki olay için standart bir servis yok, o yüzden kendi 128-bit
UUID'lerimi tanımladım.

**"Sensör sürücüsünü neden bypass ettin, bu kötü pratik değil mi?"**
Zephyr sürücüsü ilgili özelliği hiç açığa çıkarmıyor — tek trigger'ı
`SENSOR_TRIG_DATA_READY`. Seçenekler: sürücüye katkı yapmak, ya da register'lara
doğrudan yazmak. Prototip aşamasında ikincisini seçtim ve sürücünün pini
sahiplenmemesi için `TRIGGER_NONE` ile birlikte kullandım — yani çakışma yok,
bilinçli bir sınır. Ürünleşirken doğru yol sürücüye upstream katkı yapmak olur.

**"Bu kodun en zayıf yeri ne?"**
Üç şey: (1) çözülmemiş bellek bozulması, (2) disconnect'te reboot, (3) güç
durum makinesi yok — CPU sürekli uyanık, tasarımın en iddialı kısmı henüz
uygulanmadı.

---

## 11. nRF54L15 → nRF91 geçişi

Mülakat nRF91 kullanan bir firmada, "siz BLE yapmışsınız" sorusu gelecek.

**Aynen taşınan bilgi (işin çoğu):**
- Zephyr RTOS: thread, work queue, mutex, semaphore, ISR kuralları
- Devicetree ve overlay mantığı, `gpio_dt_spec`, `DT_ALIAS`
- Kconfig ve `prj.conf` katmanlaması
- **sysbuild** — nRF91'de de çoklu imaj var
- MCUboot, imzalı OTA, MCUmgr/SMP — nRF91'de DFU **modem üzerinden** yapılır
  (FOTA), ama bootloader ve imza zinciri aynı
- Sensör entegrasyonu, I²C/SPI, init öncelikleri
- Hata ayıklama: `addr2line`, map dosyası, fault dump okuma

**Farklı olan:**
- **Modem.** nRF91'de radyo ayrı bir çekirdekte kapalı bir modem firmware'i
  olarak çalışır, `nrf_modem` kütüphanesi ve AT komutları üzerinden konuşulur.
  BLE'de olduğu gibi doğrudan bir controller yok.
- **Güç profili tamamen farklı.** LTE-M/NB-IoT'de PSM ve eDRX var; tasarruf
  BLE'deki advertising aralığı ayarından çok farklı bir eksende.
- **TF-M / güvenli alan.** nRF91 Cortex-M33'ün TrustZone'unu kullanıyor, secure
  ve non-secure imaj ayrımı var (`_ns` board hedefleri).
- **GNSS** modemle paylaşımlı çalışıyor, zamanlama koordinasyonu gerektiriyor.

**Söylenecek dürüst cümle:** *"Modem tarafı benim için yeni, ama Zephyr'in
altyapısı — devicetree, Kconfig, sysbuild, work queue modeli, MCUboot zinciri —
birebir aynı. Bu projede güç tasarımını sensörün kendi motorlarına devrederek
MCU'yu işin dışında tuttum; PSM/eDRX ile çalışan bir sistemde de mesele aynı:
hangi işi hangi donanıma yaptırıp CPU'yu ne kadar uyutabildiğin."*

---

## 12. Son kontrol listesi

Mülakattan önce şunları kendi cümlelerinle anlatabildiğinden emin ol:

- [ ] Init öncelikleri neden 60'a koydum
- [ ] ISR'da neyi yapmam, neden
- [ ] `SLOPE_FDS` biti ne işe yarıyor
- [ ] Advertising 31 bayt hesabı
- [ ] RAS ile kendi profilimin farkı
- [ ] `SWAP_USING_MOVE` neden `OVERWRITE_ONLY` değil
- [ ] RF anahtarı hikâyesi ve ölçülen sonuç
- [ ] MPU fault teşhis adımları ve neden hâlâ çözülmedi
- [ ] Kodun zayıf yerleri (kendin söyle, sorulmasını bekleme)
