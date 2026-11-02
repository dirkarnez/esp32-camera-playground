%USERPROFILE%\Downloads\esptool-v5.3.1-windows-amd64\esptool-windows-amd64\esptool.exe ^
--chip esp32s3 ^
--before default-reset ^
--after hard-reset ^
write-flash ^
--flash-mode dio --flash-freq 80m --flash-size 4MB ^
0x0 bootloader/bootloader.bin ^
0x10000 partition_table/partition-table.bin ^
0x20000 camera_example.bin &&^
echo ok &&^
pause

