#!/bin/bash

# ====== 日志输出到文件 ======
LOG_FILE="/root/bt_diag_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee "$LOG_FILE") 2>&1

echo "=========================================="
echo "AP6212 Bluetooth Advanced Diagnostic"
echo "Date: $(date)"
echo "Log file: $LOG_FILE"
echo "=========================================="
echo ""

# ====== 诊断 1: 查找固件文件 ======
echo "[Diagnostic 1] Searching for BCM firmware files..."
echo "--- /system/etc/firmware ---"
ls -la /system/etc/firmware/ 2>/dev/null
echo "--- Full system search for .hcd files ---"
find / -name "*.hcd" 2>/dev/null
echo ""

# ====== 诊断 2: 蓝牙 GPIO / 设备树 ======
echo "[Diagnostic 2] Bluetooth GPIO / Device Tree..."
echo "--- wireless-bluetooth device tree ---"
for f in /proc/device-tree/wireless-bluetooth/*; do
    echo -n "  $(basename $f) = "
    cat "$f" 2>/dev/null | strings
done
echo ""
echo "--- GPIO sysfs status ---"
ls /sys/class/gpio/ 2>/dev/null
echo ""

# ====== 诊断 3: UART 状态 ======
echo "[Diagnostic 3] UART configuration..."
ls -la /dev/ttyS*
stty -F /dev/ttyS1 2>/dev/null || echo "Cannot query /dev/ttyS1 settings"
echo ""

# ====== 诊断 4: 清理 ======
echo "[Diagnostic 4] Cleanup..."
killall brcm_patchram_plus1 2>/dev/null
killall hciattach 2>/dev/null
hciconfig hci0 down 2>/dev/null
sleep 2
echo "Done."
echo ""

# ====== 诊断 5: 操作 BT GPIO（复位 + 唤醒）======
# 从设备树得知: BT,reset_gpio=79, BT,wake_gpio=81
BT_RESET_GPIO=79
BT_WAKE_GPIO=81

echo "[Diagnostic 5] Toggling BT GPIO pins..."
echo "  BT_RESET_GPIO=$BT_RESET_GPIO, BT_WAKE_GPIO=$BT_WAKE_GPIO"

# 导出并操作 reset GPIO
echo "  Exporting GPIO $BT_RESET_GPIO (reset)..."
echo $BT_RESET_GPIO > /sys/class/gpio/export 2>/dev/null
echo out > /sys/class/gpio/gpio${BT_RESET_GPIO}/direction 2>/dev/null

echo "  Exporting GPIO $BT_WAKE_GPIO (wake)..."
echo $BT_WAKE_GPIO > /sys/class/gpio/export 2>/dev/null
echo out > /sys/class/gpio/gpio${BT_WAKE_GPIO}/direction 2>/dev/null

# 复位序列: LOW -> 等待 -> HIGH
echo "  Reset sequence: LOW..."
echo 0 > /sys/class/gpio/gpio${BT_RESET_GPIO}/value 2>/dev/null
sleep 0.5

echo "  Wake: HIGH..."
echo 1 > /sys/class/gpio/gpio${BT_WAKE_GPIO}/value 2>/dev/null
sleep 0.2

echo "  Reset sequence: HIGH..."
echo 1 > /sys/class/gpio/gpio${BT_RESET_GPIO}/value 2>/dev/null
sleep 1

echo "  GPIO status after reset:"
echo "    reset(${BT_RESET_GPIO}) = $(cat /sys/class/gpio/gpio${BT_RESET_GPIO}/value 2>/dev/null || echo 'N/A')"
echo "    wake(${BT_WAKE_GPIO})  = $(cat /sys/class/gpio/gpio${BT_WAKE_GPIO}/value 2>/dev/null || echo 'N/A')"
echo ""

# ====== 诊断 6: 也尝试 rfkill 解锁 ======
echo "[Diagnostic 6] Trying rfkill unblock..."
if command -v rfkill &>/dev/null; then
    rfkill unblock bluetooth 2>/dev/null
    rfkill list
else
    echo "  rfkill not available, trying sysfs..."
    # 尝试通过 sysfs 解锁蓝牙
    for rf in /sys/class/rfkill/rfkill*; do
        TYPE=$(cat "$rf/type" 2>/dev/null)
        if [ "$TYPE" = "bluetooth" ]; then
            echo "  Found bluetooth rfkill: $rf"
            echo "    state=$(cat $rf/state 2>/dev/null)"
            echo "    soft=$(cat $rf/soft 2>/dev/null)"
            echo "    hard=$(cat $rf/hard 2>/dev/null)"
            echo 0 > "$rf/soft" 2>/dev/null
            echo "    After unblock: state=$(cat $rf/state 2>/dev/null)"
        fi
    done
fi
echo ""

# ====== 诊断 7: 使用 brcm_patchram_plus1 初始化（带固件 + GPIO 复位后）======
echo "[Diagnostic 7] Testing brcm_patchram_plus1 after GPIO reset..."

PATCHRAM=""
for fw in /system/etc/firmware/BCM43430A1.hcd /lib/firmware/brcm/*.hcd /etc/firmware/*.hcd; do
    if [ -f "$fw" ]; then
        echo "Using firmware: $fw"
        PATCHRAM="--patchram $fw"
        break
    fi
done

for BAUD in 115200 1500000; do
    echo ""
    echo "========== Testing baudrate: $BAUD =========="

    killall brcm_patchram_plus1 2>/dev/null
    hciconfig hci0 down 2>/dev/null
    sleep 1

    # 每次测试前重新复位芯片
    echo 0 > /sys/class/gpio/gpio${BT_RESET_GPIO}/value 2>/dev/null
    sleep 0.5
    echo 1 > /sys/class/gpio/gpio${BT_RESET_GPIO}/value 2>/dev/null
    sleep 1

    echo "Executing: brcm_patchram_plus1 -d $PATCHRAM --baudrate $BAUD --enable_hci --no2bytes --use_baudrate_for_download /dev/ttyS1"
    /usr/bin/brcm_patchram_plus1 \
      -d \
      $PATCHRAM \
      --baudrate $BAUD \
      --enable_hci \
      --no2bytes \
      --use_baudrate_for_download \
      /dev/ttyS1 2>&1 &

    BRCM_PID=$!
    echo "PID: $BRCM_PID"

    for i in {1..15}; do
        sleep 1

        if ! ps -p $BRCM_PID > /dev/null 2>&1; then
            echo "  Process exited at $i seconds"
            break
        fi

        if [ $((i % 3)) -eq 0 ]; then
            echo "  [$i/15] Checking hci..."
            hciconfig hci0 2>/dev/null && echo "  >>> HCI device found <<<"
        fi
    done

    # 尝试 bring up
    if hciconfig hci0 up 2>/dev/null; then
        echo ">>> SUCCESS: hci0 UP with baudrate $BAUD <<<"
        hciconfig -a
        hcitool dev
        break
    else
        echo "Failed with baudrate $BAUD"
    fi

    killall brcm_patchram_plus1 2>/dev/null
    sleep 2
done

echo ""
echo "[Diagnostic 8] Final kernel logs..."
dmesg | grep -i "bluetooth\|hci\|ttyS1\|brcm\|bcm\|gpio\|rfkill" | tail -40

echo ""
echo "=========================================="
echo "Diagnostic completed."
echo "Log saved to: $LOG_FILE"
echo "=========================================="