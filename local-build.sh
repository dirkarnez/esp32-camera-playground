#!/bin/bash

export ESP_MCU="esp32s3"

# consider espressif/esp_csi_gain_ctrl when upgrading
export IDF_VERSION_USING="ESP-IDF v6.1-dev-7351-ge37a7ae137c"

# espressif/idf:release-v6.0

docker run -t \
    -e IDF_TARGET="$ESP_MCU" \
    -v "$PWD:/app/project" \
    -w "/app/project" \
    espressif/idf:release-v6.1 /bin/bash -c "git config --global --add safe.directory '*' && \
    current_idf_version=\$(idf.py --version) && \
    ([[ \"$IDF_VERSION_USING\" == \$current_idf_version ]] && \
    echo \"Same IDF version \$current_idf_version\" || echo \"IDF has been updated to \$current_idf_version\") && \
    idf.py set-target $ESP_MCU && \
    idf.py fullclean && \
    idf.py build"

# idf.py  build
# idf.py add-dependency "espressif/esp_csi_gain_ctrl^0.1.5" && \
# idf.py set-target esp32c5 &&
# idf.py fullclean && 
# idf.py menuconfig && 
# &&  idf.py add-dependency "espressif/esp_csi_gain_ctrl^0.1.5"
# sudo rm -rf build
# idf.py add-dependency "espressif/esp_csi_gain_ctrl^0.1.5"
# idf.py save-defconfig