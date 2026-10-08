    该文件夹下面存放工程文件，包括：
    1，MDK-ARM：Keil MDK 工程（atk_f103.uvprojx，Arm Compiler 6 / ARMCLANG V6.24）

    编译产物不在这里，而是输出到工程根目录的 Output\ 下（与正点原子官方例程一致）。

    注意：atk_f103.uvprojx 在 STM32CubeMX 重新生成代码时会被按默认路径重写，
          重新生成后需要按 项目说明.md 第 6 节的表格，把包含路径、文件路径、输出目录、
          分组名再打一遍补丁，否则会找不到 Drivers\BSP、Drivers\SYSTEM、User\App 下的源码。
