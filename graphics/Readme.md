# vk_probe.c
небольшая программа для проверки Vulkan-драйвера в OpenHarmony. Она создаёт VkInstance через загрузчик Vulkan OpenHarmony и перечисляет доступные GPU. Для каждого GPU выводит имя, драйвер и версию Vulkan API, проверяет наличие расширений, важных для Render Service (VK_OHOS_native_buffer, VK_OHOS_external_memory, VK_EXT_external_memory_dma_buf, расширения синхронизации через fd, модификаторы форматов DRM, swapchain), и пробует создать VkDevice с очередью графики. Программа собирается компилятором OpenHarmony под musl, запускается как обычный процесс системы и не требует экрана. Код возврата ненулевой, если загрузчик не нашёл ни одного устройства или не удалось создать логическое устройство.

## Сборка
В контейнере перейдите в папку `/home/openharmony/oneir/tests/graphics` и выполните следующую команду:
```
R=/home/openharmony
LOADER=$R/out/oneir/thirdparty/vulkan-loader/libvulkan.so
$R/prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang \
  --target=x86_64-linux-ohos --sysroot=$R/out/oneir/obj/third_party/musl -fuse-ld=lld -O2 \
  -I$R/third_party/vulkan-headers/include \
  $R/oneir/tests/graphics/vk_probe.c -L$(dirname $LOADER) -l:libvulkan.so \
  -o $R/oneir/tests/graphics/vk_probe
```

## Запуск
На хосте запустите:
```
sudo VK_ICD_FILENAMES=/system/etc/vulkan/icd.d/intel_icd.x86_64.json \
  /mnt/ohos/OpenHarmont7/oneir/tests/graphics/vk_probe
```
