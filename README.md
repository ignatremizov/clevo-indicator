# Clevo Laptop Fan Speed Utility

Fan speed utility for Clevo laptops, supports CPU/GPU.

Based on:

- https://github.com/davidrohr/clevo-indicator
- https://github.com/SkyLandTW/clevo-indicator

Recent changes:

- Unlocked manual GUI fan minimum to 40%.
- Expanded documentation and exposed additional tuning parameters.

## Build

```bash
# For Ubuntu/Debian
sudo apt install libappindicator3-dev libgtk-3-dev
git clone https://github.com/gzzchh/clevo-indicator
cd clevo-indicator
make install
```

```bash
# For CentOS/Fedora
sudo dnf install \
libappindicator-gtk3-devel \
libappindicator-gtk3 \
libappindicator \
libappindicator-devel
git clone https://github.com/gzzchh/clevo-indicator
cd clevo-indicator
make install
```

## Usage

clevo-indicator set [fan-duty-percentage(int)] Set CPU fan percentage  
clevo-indicator setg [fan-duty-percentage(int)] Set GPU fan percentage  
clevo-indicator help Show help  
clevo-indicator dump Get temperatures  
clevo-indicator dumpall Get raw temperature data  
clevo-indicator auto Auto-adjust fan speed and exit (for scheduled scripts)  
clevo-indicator indicator Show GUI fan control

fan-duty-percentage is an integer value  
Specifies target fan duty percentage.

## Contributing

Development workflow and commit message conventions are documented in [CONTRIBUTING.md](CONTRIBUTING.md).

## Note

The EC interface requires root privileges, while most Linux desktop sessions run
as normal users, so this binary needs setuid.

Set it up like this:

```bash
sudo chown root clevo-indicator
sudo chmod u+s  clevo-indicator
```

This program conflicts with other tools that access EC through low-level calls,
and may cause unstable behavior.  
There is no additional protection around EC access in this project.

The executable has setuid flag on, but must be run by the current desktop user,
because only the desktop user is allowed to display a desktop indicator in
Ubuntu, while a non-root user is not allowed to control Clevo EC by low-level
IO ports. The setuid=root creates a special situation in which this program can
fork itself and run under two users (one for desktop/indicator and the other
for EC control), so you could see two processes in ps, and killing either one
of them would immediately terminate the other.

Be careful not to use any other program accessing the EC by low-level IO
syscalls (inb/outb) at the same time - I don't know what might happen, since
every EC actions require multiple commands to be issued in correct sequence and
there is no kernel-level protection to ensure each action must be completed
before other actions can be performed... The program also attempts to prevent
abortion while issuing commands by catching all termination signals except
SIGKILL - don't kill the indicator by "kill -9" unless absolutely necessary.

## Hacking

To add more GUI menu options, edit the array around line ~138 in
`clevo-indicator.c` and extend it as needed.

```c
static menuitems[] = {
    {"Set FAN to AUTO", G_CALLBACK(ui_command_set_fan), 0, AUTO, NULL},
    {"", NULL, 0L, NA, NULL},
    {"Set FAN to  40%", G_CALLBACK(ui_command_set_fan), 40, MANUAL, NULL},
    {"Set FAN to  50%", G_CALLBACK(ui_command_set_fan), 50, MANUAL, NULL},
    {"Set FAN to  60%", G_CALLBACK(ui_command_set_fan), 60, MANUAL, NULL},
    {"Set FAN to  70%", G_CALLBACK(ui_command_set_fan), 70, MANUAL, NULL},
    {"Set FAN to  80%", G_CALLBACK(ui_command_set_fan), 80, MANUAL, NULL},
    {"Set FAN to  90%", G_CALLBACK(ui_command_set_fan), 90, MANUAL, NULL},
    {"Set FAN to 100%", G_CALLBACK(ui_command_set_fan), 100, MANUAL, NULL},
    {"", NULL, 0L, NA, NULL},
    {"Quit", G_CALLBACK(ui_command_quit), 0L, NA, NULL}};
```

Duty/auto-control rules are implemented in `clevo-indicator.c` around line ~884, for example:

```c
static int ec_auto_duty_adjust(void)
{
    int temp = MAX(share_info->cpu_temp, share_info->gpu_temp);
    int duty = share_info->fan_duty;
    //
    if (temp >= 80 && duty < 100)
        return 100;
    // Additional logic omitted for brevity in this example.
    //
    return 0;
}
```
