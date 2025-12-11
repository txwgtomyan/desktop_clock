
# 创建工程

1. 创建一个hello_word的示例工程
2. 重命名hello_word_main.c
3. 删除工程`CMakeLists.txt`中的`idf_build_set_property(MINIMAL_BUILD ON)`，否则无法配置PSRAM

# 修改SPI_FLASH和PSRAM

## 修改RSRAM
1. 设置`SPI RAM config`的`Mode (QUAD/OCT) of SPI RAM chip in use`为`**Octal Mode PSRAM**`
2. 设置`Set RAM clock speed`为`80M clock speed`

## 修改SPI_FLASH

1. 设置`Serial flasher config`的`flash size`为16M

# 添加外部组件

在`main`文件夹下面添加`idf_component.yml`文件，文件内容如下：

```YML
## IDF Component Manager Manifest File
dependencies:
  ## Required IDF version
  idf:
    version: '>=4.1.0'
  # # Put list of dependencies here
  # # For components maintained by Espressif:
  # component: "~1.0.0"
  # # For 3rd party components:
  # username/component: ">=1.0.0,<2.0.0"
  # username2/component2:
  #   version: "~1.0.0"
  #   # For transient dependencies `public` flag can be set.
  #   # `public` flag doesn't have an effect dependencies of the `main` component.
  #   # All dependencies of `main` are public by default.
  #   public: true
  espressif/esp_lcd_axs15231b: ^1.0.1
  espressif/esp_io_expander_tca9554: ^2.0.1

```

外部组件会在编译的时候进行下载

# 工程格式

```bash
.
├── CMakeLists.txt
├── components
│   └── board_bsp
│       ├── board_bsp.c
│       ├── board_bsp.h
│       └── CMakeLists.txt
├── dependencies.lock
├── main
│   ├── app_main.c
│   ├── board_config.h
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── README.md
├── sdkconfig
└── sdkconfig.old
```

`components`是组件的意思，实现某种功能，board_bsp.c中要引用`#include "driver/i2c_master.h"`需要在`board_bsp/CMakeLists.txt`中添加`REQUIRES driver`，引用`main/board_config.h`需要在`board_bsp/CMakeLists.txt`中添加`PRIV_INCLUDE_DIRS "../../main"`
