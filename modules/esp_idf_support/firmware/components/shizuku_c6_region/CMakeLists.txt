# C6 U-mode + PMP (ESP32-C6)。SHIZUKU_C6_UMODE=on の時だけ中身を持つ (既定 on)。
# project の components/ は IDF が既定で探すため、off の時は空のコンポーネントとして登録する。
# on の時: IDF の esp_cpu_configure_region_protection を --wrap で差し替える。PMA のみを設定し、PMP には書かない。
# bootloader subproject は -DSHIZUKU_C6_UMODE を受け取らないため、判定は BOOTLOADER_BUILD も見る。
# この directory が bootloader へ渡るのは flag が on の時だけなので、bootloader subproject 内なら on とみなす。
if(BOOTLOADER_BUILD OR SHIZUKU_C6_UMODE STREQUAL "on")
  idf_component_register(SRCS "shizuku_c6_region.c"
                         INCLUDE_DIRS "."
                         REQUIRES bootloader_support esp_hw_support)
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_cpu_configure_region_protection")
  # static archive の member を link に含めるため、未定義参照を -u で先に作る (未検証: Sol の rbuild の app link で確認する)。
  target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,__wrap_esp_cpu_configure_region_protection")
  # marker は非 bootloader 限定の定義 (shizuku_c6_region.c の #if !defined(BOOTLOADER_BUILD))。bootloader では -u を付けない (未検証)。
  if(NOT BOOTLOADER_BUILD)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,shizuku_c6_stage1_pma_ready")
  endif()
else()
  idf_component_register()
endif()
