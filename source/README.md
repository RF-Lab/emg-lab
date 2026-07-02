# Baseline code for ADS1299 initialization

### Hardware Required

Для работы требуется плата myocell_s3

### Configure the project

Для сборки микропрограммного обеспечения необходимо установить ESP-IDF [Инструкция](http://rf-lab.org/news/2020/04/04/esp-idf.html).
Выбрать микроконтроллер
```
idf.py set-target esp32s3
```
Для настройки параметров прошивки используется команда 
```
idf.py menuconfig
```
Необходимо выбрать канальность АЦП в разделе MyoCell Configuration

Режим работы платы (Inference/RawData) можно поменять кнопкой PROG

Также следует проверить слудующие настройки, и при необходимости установить их в соответсвии с примерами ниже:

![ClockConfig](https://drive.google.com/file/d/13Jj6xhyjLkEHcpFTvao6gjBG-8tc5EuW/view?usp=sharing)

![ClockConfig](https://drive.google.com/file/d/1onmu_HltZbPhOJho_KuUCeFca55VGnIC/view?usp=sharing)

![ClockConfig](https://drive.google.com/file/d/1ldYfmj5YujeDsoiEr0PYwSS34gvEyrvU/view?usp=sharing)

![ClockConfig](https://drive.google.com/file/d/1ER45dmp5zdVaNlV5QWKfAEBfHGRzCfRw/view?usp=sharing)

![ClockConfig](https://drive.google.com/file/d/1HnuJOCX7_MDm4HRtw1LrAh7Iw4i-qFRK/view?usp=sharing)

### Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

