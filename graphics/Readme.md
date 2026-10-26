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


## Подготовка

- Запущены службы стенда: `ohos-hilogd`, `ohos-param`, `ohos-samgr`, `ohos-atm`, `ohos-param-watcher`,
  `ohos-hdf-devmgr`, `ohos-allocator-host`, `ohos-composer-host`, `ohos-render-service`.
- `kernel.pid_max ≤ 65536` (иначе VSync в RS перестаёт работать после роста PID).
- Переменная окружения для всех запусков:
  `E="VK_ICD_FILENAMES=/system/etc/vulkan/icd.d/intel_icd.x86_64.json"`

| Тест | Как запустить | Что проверяет |
|---|---|---|
| `screen` | `sudo env $E rs_probe screen` | Экран по умолчанию, активный режим (1920×1080), список режимов, яркость, включение питания экрана |
| `colors` | `sudo env $E rs_probe colors` | Заливка окна красным, зелёным, синим, белым, серым, чёрным; сверка цвета в центре кадра — правильный порядок каналов во всей цепочке RS → Vulkan → снимок |
| `windows` | `sudo env $E rs_probe windows` | Три перекрывающихся окна разных размеров: порядок по Z, скругление угла, изменение размера окна, прозрачность 0,5 |
| `shapes` | `sudo env $E rs_probe shapes` | Прямоугольник, круг, линейный градиент через запись команд на холсте узла — рендеринг самим RS на GPU |
| `text` | `sudo env $E rs_probe text` | Вывод строк четырёх размеров; в области текста есть светлые пиксели (наличие шрифтов в системе) |
| `cpubuffer` | `sudo env $E rs_probe cpubuffer` | Заливка на CPU прямо в буфер поверхности (`RequestBuffer` → `FlushBuffer`): путь готовых буферов allocator → RS → композиция |
| `vsync` | `sudo env $E rs_probe vsync` | Приёмник VSync клиента: число сигналов за 2 с, период (ожидается ≈ 16,65 мс при 60 Гц), макс. отклонение |
| `perf` | `sudo env $E rs_probe perf` | Анимация квадрата по сигналам VSync (240 шагов, один шаг на сигнал): частота кадров, период VSync относительно частоты экрана, пропуски VSync (тест не пройден при любом пропуске), макс. время отправки транзакции; задержка снимка экрана |
| `stress` | `sudo env $E rs_probe stress [--grid N]` | Сетка N×N узлов (по умолчанию 20×20 = 400): время создания и отправки, задержка снимка, сверка цвета ячейки в середине сетки |
| `single` | `sudo env $E rs_probe single` | Диагностический, в общий прогон не входит: один узел с вариациями размера, цвета и точки проверки — для поиска расхождений между тестами |

### Опции rs_probe

| Опция | Назначение |
|---|---|
| `-h` | Список тестов и опций |
| `--hold <с>` | Сколько секунд держать каждую картинку на экране (по умолчанию 1) |
| `--grid N` | Размер сетки для `stress` (N×N, от 1 до 60) |
| `--no-capture` | Без снимков экрана: только вывод на экран, проверки по снимкам считаются пройденными |
| `--attach-to-display` | Прикреплять окно через `AttachToDisplay` вместо `RSDisplayNode` (для сравнения; на стенде без службы дисплеев окно при этом не выводится на экран) |



## Сборка
Для сборки тестов выполните команду `./make.sh --reload //oneir/tests/graphics:graphics_tests` в контейнере. Результат компиляции находится по пути `out/oneir/oneir/oneir_tests/rs_probe`. 

Запуск следующими командами:
```bash
sudo VK_ICD_FILENAMES=/system/etc/vulkan/icd.d/intel_icd.x86_64.json ./rs_probe            # все тесты
sudo VK_ICD_FILENAMES=/system/etc/vulkan/icd.d/intel_icd.x86_64.json ./rs_probe colors --hold 3
./rs_probe -h
```
