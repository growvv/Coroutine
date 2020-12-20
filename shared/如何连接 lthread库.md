## 如何连接 `lthread`库

1. 生成静态链接库

   ```bash
   git clone https://github.com/halayli/lthread.git
   cd lthread
   cmake .
   sudo make install
   ```

   注意到，在当前文件夹生成了`liblthread.a`

2. 编译测试程序

   测试程序：https://lthread.readthedocs.io/en/latest/examples.html

   ```bash
   gcc webserver.c liblthread.a -o webserver -l pthread -w
   sudo ./webserver
   ```

   