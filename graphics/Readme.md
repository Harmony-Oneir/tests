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

# rs_probe.cpp

Строит дерево экран → окно → содержимое и проверяет результат снимком экрана через RS: читает пиксели в нужных точках и сверяет цвета. Поэтому она сама скажет, что прошло, а что нет, даже если экран ноутбука остаётся чёрным.

| Тест |	Что проверяет |
|------|----------------|
| screen | экран по умолчанию, активный режим, список режимов, яркость, включение питания |
| colors | заливки окна красным, зелёным, синим, белым, серым, чёрным, со сверкой центра кадра (сразу видно перепутанные каналы) |
| windows |три перекрывающихся окна разных размеров: порядок по Z, скругление угла, изменение размера, прозрачность 50 % |
| shapes |прямоугольник, круг и линейный градиент через запись команд на холсте (рисует сам RS на GPU через Vulkan) |
| text	| строки четырёх размеров; проверяет, что в области текста появились светлые пиксели (если нет шрифтов в системе, тест так и скажет) |
| cpubuffer	| заливка на CPU прямо в буфер поверхности, как в drawing_sample_rs: RS только компонует готовый буфер |
| vsync	частота и стабильность сигналов VSync за 2 секунды |
| perf |	240 кадров перемещения узла: фактические кадры в секунду, максимальное время отправки транзакции, задержка снимка экрана |
| stress |	400 узлов-прямоугольников: время создания и отправки, задержка снимка, сверка одной из ячеек |


## Сборка
Для сборки тестов выполните команду `./make.sh --reload //oneir/tests/graphics:graphics_tests` в контейнере. Результат компиляции находится по пути `out/oneir/oneir/oneir_tests/rs_probe`. 

Запуск следующими командами:
```bash
sudo VK_ICD_FILENAMES=/system/etc/vulkan/icd.d/intel_icd.x86_64.json ./rs_probe            # все тесты
sudo VK_ICD_FILENAMES=/system/etc/vulkan/icd.d/intel_icd.x86_64.json ./rs_probe colors --hold 3
./rs_probe -h
```
