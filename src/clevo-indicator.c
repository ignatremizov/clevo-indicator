/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================

 TEST:
 gcc clevo-indicator.c -o clevo-indicator `pkg-config --cflags --libs gtk+-3.0 ayatana-appindicator3-0.1` -lm
 sudo chown root clevo-indicator
 sudo chmod u+s clevo-indicator

 Run as effective uid = root, but uid = desktop user (in order to use indicator).

 ============================================================================
 Auto fan control algorithm:

 The algorithm is to replace the builtin auto fan-control algorithm in Clevo
 laptops which is apparently broken in recent models such as W350SSQ, where the
 fan doesn't get kicked until both of GPU and CPU are really hot (and GPU
 cannot be hot anymore thanks to nVIDIA's Maxwell chips). It's far more
 aggressive than the builtin algorithm in order to keep the temperatures below
 60°C all the time, for maximized performance with Intel turbo boost enabled.

 ============================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <gtk/gtk.h>
#include <cairo/cairo.h>
#include <libayatana-appindicator/app-indicator.h>

#include "sni.h"
#include "ec-monitor.h"

#define NAME "clevo-indicator"

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define P775DM3 //THIS IS THE MODEL DEFINITION, TO FIND THE ADDRESSES IN THE EC

#define EC_REG_SIZE 0x100

#define EC_REG_CPU_FAN_DUTY 0xCE
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_CPU_FAN_RPMS_HI 0xD0
#define EC_REG_CPU_FAN_RPMS_LO 0xD1
#define EC_REG_GPU_FAN_RPMS_HI 0xD2
#define EC_REG_GPU_FAN_RPMS_LO 0xD3
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_GPU_FAN_DUTY 0xCF

#define MAX_FAN_RPM 4400.0

#define TEMP_FAIL_THRESHOLD 15

typedef enum
{
    NA = 0,
    AUTO = 1,
    MANUAL = 2
} MenuItemType;

typedef enum
{
    FAN_CPU = 1,
    FAN_GPU = 2
} FanIndex;

#define FAN_COMMAND(fan, duty) (((fan) << 8) | ((duty)&0xff))
#define FAN_CMD_FAN(cmd) (((cmd) >> 8) & 0xff)
#define FAN_CMD_DUTY(cmd) ((cmd)&0xff)

int use_hwmon_interface = 0;
int hwmon_interface_num = 0;
static int use_gpu_temp_smi = 1;

static void main_init_share(void);
static int main_ec_worker(void);
static void main_ui_worker(int argc, char **argv);
static void main_on_sigchld(int signum);
static void main_on_sigterm(int signum);
static int main_dump_fan(void);
static int main_test_cpu_fan(int duty_percentage);
static int main_test_gpu_fan(int duty_percentage);
static gboolean ui_update(gpointer user_data);
static void ui_on_reconnect(AppIndicator *ind, gboolean connected, gpointer data);
static void ui_sni_activate(int x, int y, void *user_data);
static void ui_sni_context_menu(int x, int y, void *user_data);
static void ui_sni_secondary_activate(int x, int y, void *user_data);
static void ui_command_set_fan(long fan_duty);
static void ui_fan_item_activated(GtkMenuItem *item, gpointer data);
static void ui_fan_btn_clicked(GtkWidget *btn, gpointer data);
static GdkPixbuf *ui_draw_fan_icon(int size);
static const char *ui_setup_icon_theme(void);
static void ui_command_quit(gchar *command);
static const char *ui_popup_row_class(int duty, MenuItemType type);
static void ui_popup_init(void);
static void ui_popup_show_at(int x, int y);
static void ui_popup_hide(void);
static gboolean ui_popup_focus_out(GtkWidget *widget, GdkEventFocus *event,
                                   gpointer user_data);
static gboolean ui_popup_key_press(GtkWidget *widget, GdkEventKey *event,
                                   gpointer user_data);
static void ui_toggle_menuitems(void);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_auto_duty_adjust(FanIndex fan, int temp, int current_duty);
static void ec_reset_auto_duty_state(FanIndex fan);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_gpu_temp_nvidia(void);
static int ec_query_cpu_fan_duty(void);
static int ec_query_cpu_fan_rpms(void);
static int ec_query_gpu_fan_duty(void);
static int ec_query_gpu_fan_rpms(void);
static int ec_write_cpu_fan_duty(int duty_percentage);
static int ec_write_gpu_fan_duty(int duty_percentage);
static int ec_io_wait(const uint32_t port, const uint32_t flag,
                      const char value);
static int ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port,
                    const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int check_proc_instances(const char *proc_name);
static void get_time_string(char *buffer, size_t max, const char *format);
static void signal_term(__sighandler_t handler);

static AppIndicator   *indicator_cpu    = NULL;  /* CPU fan control */
static AppIndicator   *indicator_gpu    = NULL;  /* GPU fan control */
static ClevoSni       *experimental_sni = NULL;  /* Default native SNI path */
static gboolean        ui_use_experimental_sni = FALSE;
static GtkWidget      *fan_popup_window = NULL;
static char ui_last_label[256] = "";
#define MAX_FAN_ROWS 16

typedef struct
{
    const char *label;
    int duty;
    MenuItemType type;
    GtkWidget *cpu_item;  /* GtkMenuItem in the CPU fan indicator menu */
    GtkWidget *gpu_item;  /* GtkMenuItem in the GPU fan indicator menu */
    GtkWidget *cpu_btn;   /* GtkButton in the CPU popup column */
    GtkWidget *gpu_btn;   /* GtkButton in the GPU popup column */
} FanControlRow;

static FanControlRow control_rows[] = {
    {"AUTO", 0, AUTO,   NULL, NULL, NULL, NULL},
    {"40%",  40, MANUAL, NULL, NULL, NULL, NULL},
    {"50%",  50, MANUAL, NULL, NULL, NULL, NULL},
    {"60%",  60, MANUAL, NULL, NULL, NULL, NULL},
    {"70%",  70, MANUAL, NULL, NULL, NULL, NULL},
    {"80%",  80, MANUAL, NULL, NULL, NULL, NULL},
    {"90%",  90, MANUAL, NULL, NULL, NULL, NULL},
    {"100%", 100, MANUAL, NULL, NULL, NULL, NULL},
};

static int control_rows_count = (sizeof(control_rows) / sizeof(control_rows[0]));

typedef struct
{
    int temp;
    int duty;
} AutoFanCurvePoint;

typedef struct
{
    int initialized;
    int last_applied_duty;
    int last_temp;
    struct timespec last_change_at;
} AutoFanState;

static AutoFanState auto_fan_states[2] = {0};

#define AUTO_FAN_EMERGENCY_TEMP_C 90
#define AUTO_FAN_MIN_ADJUST_MS 3000
#define AUTO_FAN_MIN_DUTY_DELTA 3
#define AUTO_FAN_UP_HYSTERESIS_C 2
#define AUTO_FAN_DOWN_HYSTERESIS_C 3

/* Piecewise-linear shared fan curve. Both fans follow the hotter of CPU/GPU
   because the Clevo chassis and cooling path are thermally coupled. */
static const AutoFanCurvePoint auto_fan_curve[] = {
    {15, 30},
    {50, 42},
    {60, 52},
    {68, 62},
    {75, 74},
    {82, 88},
    {88, 100},
};

static int auto_fan_curve_count =
    (sizeof(auto_fan_curve) / sizeof(auto_fan_curve[0]));

static struct
{
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int cpu_fan_duty;
    volatile int gpu_fan_duty;
    volatile int cpu_fan_rpms;
    volatile int gpu_fan_rpms;
    volatile int auto_cpu_duty;
    volatile int auto_gpu_duty;
    volatile int auto_cpu_duty_val;
    volatile int auto_gpu_duty_val;
    volatile int manual_next_cpu_fan_duty;
    volatile int manual_next_gpu_fan_duty;
    volatile int manual_prev_cpu_fan_duty;
    volatile int manual_prev_gpu_fan_duty;
} *share_info = NULL;

static pid_t parent_pid = 0;
static pid_t worker_pid = 0;

void autoset_cpu_gpu()
{
    struct sched_param param;
    param.sched_priority = 99;
    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
    {
        printf("sched_setscheduler error\n");
        exit(EXIT_FAILURE);
    }

    int initial = 1;
    int current[2] = {0, 0};
    double lastCPU = 0., lastGPU = 0.;
    int repeatCheck[2] = {0, 0};
    int lastfail = 0;
    int missing = 0;

    fd_set readfds;
    FD_ZERO(&readfds);
    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    FILE *ctrl_file = NULL;

    static int ctrl_setting_offset_cpu = 0;
    static int ctrl_setting_offset_gpu = 0;
    static int ctrl_setting_min_cpu = 0;
    static int ctrl_setting_min_gpu = 0;
    static int ctrl_setting_force_cpu = -1;
    static int ctrl_setting_force_gpu = -1;

    while (1)
    {
        //printf("Checking\n");
        if (missing > 5)
        {
            ec_write_gpu_fan_duty(70);
            ec_write_cpu_fan_duty(70);
            exit(1);
        }

        char buffer[10];
        double gputemp;
        int found = 0;
        while (!feof(stdin))
        {
            FD_SET(STDIN_FILENO, &readfds);
            if (!select(1, &readfds, NULL, NULL, &timeout))
                break;
            int nRead = read(STDIN_FILENO, buffer, 3);
            //printf("Read %d\n", nRead);
            if (nRead == 0)
                break;
            found = 1;
            gputemp = atoi(buffer);
        };

        if (found)
        {
            static int ctrl_check = 0;
            if (ctrl_check++ >= 3)
            {
                ctrl_check = 0;
                ctrl_file = fopen("/tmp/clevo_fan_ctrl", "r");
                if (ctrl_file != NULL)
                {
                    while (!feof(ctrl_file))
                    {
                        char buffer[1024];
                        fgets(buffer, 1023, ctrl_file);
                        if (strncmp(buffer, "offset_cpu", 10) == 0)
                            sscanf(buffer, "offset_cpu %d", &ctrl_setting_offset_cpu);
                        if (strncmp(buffer, "offset_gpu", 10) == 0)
                            sscanf(buffer, "offset_gpu %d", &ctrl_setting_offset_gpu);
                        if (strncmp(buffer, "min_cpu", 7) == 0)
                            sscanf(buffer, "min_cpu %d", &ctrl_setting_min_cpu);
                        if (strncmp(buffer, "min_gpu", 7) == 0)
                            sscanf(buffer, "min_gpu %d", &ctrl_setting_min_gpu);
                        if (strncmp(buffer, "force_cpu", 7) == 0)
                            sscanf(buffer, "force_cpu %d", &ctrl_setting_force_cpu);
                        if (strncmp(buffer, "force_gpu", 7) == 0)
                            sscanf(buffer, "force_gpu %d", &ctrl_setting_force_gpu);
                    }
                    printf("Control settings: Offset CPU %d, Offset GPU %d, Min CPU %d, Min GPU %d, Force CPU %d, Force GPU %d (hwmon %d)\n", ctrl_setting_offset_cpu, ctrl_setting_offset_gpu, ctrl_setting_min_cpu, ctrl_setting_min_gpu, ctrl_setting_force_cpu, ctrl_setting_force_gpu, use_hwmon_interface);
                    fclose(ctrl_file);
                }
            }
            double cputemp;
            for (int tries = 0; tries < 3; tries++)
            {
                cputemp = ec_query_cpu_temp();
                if (cputemp < 100 || cputemp < lastCPU + 20)
                    break;
            }
            if (cputemp < lastCPU - 10)
                cputemp = lastCPU - 10;

            int cur_cpu_setting = ec_query_cpu_fan_duty();
            int cur_gpu_setting = ec_query_gpu_fan_duty();

            double gputemptmp;
            if (gputemp <= 65)
                gputemptmp = gputemp - 10;
            else if (gputemp < 75)
                gputemptmp = gputemp - (75 - gputemp);
            else
                gputemptmp = gputemp;

            double avg[2];
            if (cputemp > gputemptmp)
            {
                avg[0] = cputemp;
                avg[1] = (2 * gputemptmp + cputemp) / 3;
            }
            else
            {
                avg[1] = gputemptmp;
                avg[0] = (2 * cputemp + gputemptmp) / 3;
            }
            if (lastCPU > 30)
                avg[0] = (2 * avg[0] + lastCPU) / 3;
            if (lastGPU > 30)
                avg[1] = (2 * avg[1] + lastGPU) / 3;
            if (cputemp >= TEMP_FAIL_THRESHOLD)
                lastCPU = avg[0];
            if (gputemp >= TEMP_FAIL_THRESHOLD)
                lastGPU = avg[1];

            int setDuty[2];
            for (int i = 0; i < 2; i++)
            {
                if (avg[i] <= 40)
                    setDuty[i] = 0;
                else if (avg[i] <= 45)
                    setDuty[i] = 15;
                else if (avg[i] <= 75)
                    setDuty[i] = avg[i] - 30;
                else if (avg[i] <= 90)
                    setDuty[i] = (avg[i] - 75) * 3 + 45;
                else
                    setDuty[i] = 100;
            }

            if (ctrl_setting_offset_cpu)
                setDuty[0] += ctrl_setting_offset_cpu;
            if (ctrl_setting_offset_gpu)
                setDuty[1] += ctrl_setting_offset_gpu;
            if (ctrl_setting_min_cpu > setDuty[0])
                setDuty[0] = ctrl_setting_min_cpu;
            if (ctrl_setting_min_gpu > setDuty[1])
                setDuty[1] = ctrl_setting_min_gpu;
            if (ctrl_setting_force_cpu != -1)
                setDuty[0] = ctrl_setting_force_cpu;
            if (ctrl_setting_force_gpu != -1)
                setDuty[1] = ctrl_setting_force_gpu;
            for (int i = 0; i < 2; i++)
                if (setDuty[i] > 100)
                    setDuty[i] = 100;

            int doSet[2] = {0, 0};
            for (int i = 0; i < 2; i++)
            {
                if (current[i] == 0 && setDuty[i] != 0)
                    doSet[i] = 1;
                else if (setDuty[i] > current[i] && setDuty[i] > 50)
                    doSet[i] = 1;
                else if (setDuty[i] > current[i] + 1)
                    doSet[i] = 1;
                else if (setDuty[i] < current[i] - 5)
                    doSet[i] = 1;
                else if (setDuty[i] < current[i])
                {
                    if (repeatCheck[i] >= 4)
                        doSet[i] = 1;
                    else
                        repeatCheck[i]++;
                }

                if (doSet[i])
                    repeatCheck[i] = 0;
            }

            if (initial)
            {
                doSet[0] = doSet[1] = 1;
                initial = 0;
            }
            else if (cur_cpu_setting != current[0] || cur_gpu_setting != current[1])
            {
                doSet[0] = doSet[1] = 1;
                for (int i = 0; i < 2; i++)
                    if (setDuty[i] < current[i])
                        setDuty[i] = current[i];
            }

            if (cputemp < TEMP_FAIL_THRESHOLD || gputemp < TEMP_FAIL_THRESHOLD)
            {
                if (lastfail >= 1)
                {
                    doSet[0] = doSet[1] = 1;
                    if (setDuty[0] < 50)
                        setDuty[0] = 50;
                    if (setDuty[1] < 50)
                        setDuty[1] = 50;
                }
                else
                {
                    lastfail++;
                    doSet[0] = doSet[1] = 0;
                }
            }
            else
            {
                lastfail = 0;
            }

            printf("Temperatures C: %f G: %f --> %f %f --> New Duty: %d (%d) %d (%d) - Activate %d %d\n", cputemp, gputemp, avg[0], avg[1], setDuty[0], cur_cpu_setting, setDuty[1], cur_gpu_setting, doSet[0], doSet[1]);

            for (int i = 0; i < 2; i++)
            {
                if (doSet[i])
                {
                    current[i] = setDuty[i];
                    int retVal;
                    for (int j = 0; j < 3; j++)
                    {
                        if (i)
                            retVal = ec_write_gpu_fan_duty(setDuty[1]);
                        else
                            retVal = ec_write_cpu_fan_duty(setDuty[0]);
                        if (retVal == EXIT_SUCCESS)
                        {
                            usleep(1100000);
                            int new_setting = i ? ec_query_gpu_fan_duty() : ec_query_cpu_fan_duty();
                            if (new_setting == setDuty[i])
                                break;
                            printf("Mismatch %d : %d v.s. %d\n", i, new_setting, setDuty[i]);
                        }
                        printf("Error setting speed, retrying...\n");
                        usleep(50000);
                    }
                }
            }
            missing = 0;
        }
        else
        {
            missing++;
        }
        usleep(1000000);
    };
}

int main(int argc, char *argv[])
{
    printf("Simple fan control utility for Clevo laptops\n");
    if (check_proc_instances(NAME) > 1)
    {
        printf("Multiple running instances!\n");
        char *display = getenv("DISPLAY");
        if (display != NULL && strlen(display) > 0)
        {
            int desktop_uid = getuid();
            setuid(desktop_uid);
            //
            gtk_init(&argc, &argv);
            GtkWidget *dialog = gtk_message_dialog_new(NULL, 0,
                                                       GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                       "Multiple running instances of %s!", NAME);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
        return EXIT_FAILURE;
    }
    if (ec_init() != EXIT_SUCCESS)
    {
        printf("unable to control EC: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (getenv("USE_EC_GPU_TEMP") != NULL &&
        strcmp(getenv("USE_EC_GPU_TEMP"), "1") == 0)
        use_gpu_temp_smi = 0;
    if (argc <= 1 || strcmp(argv[1], "help") == 0)
    {
        printf(
            "\n"
            "Usage:\n"
            "clevo-indicator set|setg [fan-duty-percentage(int)]  Set fan (CPU|GPU) percentage\n"
            "clevo-indicator help Display help\n"
            "clevo-indicator dump Show temperatures\n"
            "clevo-indicator dumpall Show raw temperature data\n"
            "clevo-indicator auto Automatically set fan speed and exit\n"
            "clevo-indicator indicator Show fan control GUI\n"
            "\n"
            "Fan control utility for Clevo laptops, with CPU/GPU support, and default status output plus usage help.\n"
            "\n"
            "Arguments:\n"
            "  [fan-duty-percentage]\t\tTarget duty percentage, int\n"
            "  -?\t\t\t\tDisplay this help and exit\n"
            "\n"
            "Without arguments this program should attempt to display an indicator in\n"
            "the Ubuntu tray area for fan information display and control. The indicator\n"
            "requires this program to have setuid=root flag but run from the desktop user\n"
            ", because a root user is not allowed to display a desktop indicator while a\n"
            "non-root user is not allowed to control Clevo EC (Embedded Controller that's\n"
            "responsible of the fan). Fix permissions of this executable if it fails to\n"
            "run:\n"
            "    sudo chown root clevo-indicator\n"
            "    sudo chmod u+s  clevo-indicator\n"
            "\n"
            "Note any fan duty change should take 1-2 seconds to come into effect - you\n"
            "can verify by the fan speed displayed on indicator icon and also louder fan\n"
            "noise.\n"
            "\n"
            "In the indicator mode, this program would always attempt to load kernel\n"
            "module 'ec_sys', in order to query EC information from\n"
            "'/sys/kernel/debug/ec/ec0/io' instead of polling EC ports for readings,\n"
            "which may be more risky if interrupted or concurrently operated during the\n"
            "process.\n"
            "\n"
            "DO NOT MANIPULATE OR QUERY EC I/O PORTS WHILE THIS PROGRAM IS RUNNING.\n"
            "\n");
        return main_dump_fan();
    }
    else if (strcmp(argv[1], "indicator") == 0)
    {
        main_dump_fan();
        char *display = getenv("DISPLAY");
        if (display == NULL || strlen(display) == 0)
        {
            return EXIT_SUCCESS;
        }
        // Start GUI when display is available.
        else
        {
            parent_pid = getpid();
            main_init_share();
            signal(SIGCHLD, &main_on_sigchld);
            signal_term(&main_on_sigterm);
            sigset_t child_mask, previous_mask;
            sigemptyset(&child_mask);
            sigaddset(&child_mask, SIGCHLD);
            if (sigprocmask(SIG_BLOCK, &child_mask, &previous_mask) != 0)
                return EXIT_FAILURE;
            worker_pid = fork();
            if (worker_pid == 0)
            {
                signal(SIGCHLD, SIG_DFL);
                sigprocmask(SIG_SETMASK, &previous_mask, NULL);
                signal_term(&ec_on_sigterm);
                return main_ec_worker();
            }
            else if (worker_pid > 0)
            {
                /* Publish worker_pid before an immediate child exit can run
                   the handler; unrelated GPU child exits must not kill UI. */
                sigprocmask(SIG_SETMASK, &previous_mask, NULL);
                main_ui_worker(argc, argv);
                share_info->exit = 1;
                waitpid(worker_pid, NULL, 0);
            }
            else
            {
                sigprocmask(SIG_SETMASK, &previous_mask, NULL);
                printf("unable to create worker: %s\n", strerror(errno));
                return EXIT_FAILURE;
            }
        }
    }
    else if (strcmp(argv[1], "set") == 0)
    {
        if (argc < 2)
        {
            printf("Missing argument\n");
            return EXIT_FAILURE;
        }
        else
        {
            int val = atoi(argv[2]);
            if (val < 0 || val > 100)
            {
                printf("invalid fan duty %d!\n", val);
                return EXIT_FAILURE;
            }
            return main_test_cpu_fan(val);
        }
    }
    else if (strcmp(argv[1], "setg") == 0)
    {
        if (argc < 2)
        {
            printf("Missing argument\n");
            return EXIT_FAILURE;
        }
        else
        {
            int val = atoi(argv[2]);
            if (val < 0 || val > 100)
            {
                printf("invalid fan duty %d!\n", val);
                return EXIT_FAILURE;
            }
            return main_test_gpu_fan(val);
        }
    }
    else if (strcmp(argv[1], "dump") == 0)
    {
        return main_dump_fan();
    }
    else if (strcmp(argv[1], "dumpall") == 0)
    {
        int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
        if (io_fd < 0)
        {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        switch (len)
        {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            return EXIT_FAILURE;
        case 0x100:
            break;
        default:
            printf("Error reading from EC\n");
            return EXIT_FAILURE;
        }

        for (int i = 0; i < EC_REG_SIZE; i++)
        {
            printf("0x%02x: 0x%02x (%3d) ", i, buf[i], buf[i]);
            if (i == EC_REG_CPU_TEMP)
                printf("C");
            else if (i == EC_REG_GPU_TEMP)
                printf("G");
            else if (i == EC_REG_CPU_FAN_DUTY)
                printf("F");
            else if (i == EC_REG_CPU_FAN_RPMS_HI)
                printf("H");
            else if (i == EC_REG_CPU_FAN_RPMS_LO)
                printf("L");
            else if (buf[i] >= 51 && buf[i] <= 51)
                printf("X");
            else
                printf(" ");
            printf(" ");
            if ((i + 1) % 16 == 0)
                printf("\n");
        }
        close(io_fd);
    }
    else if (strcmp(argv[1], "auto") == 0)
    {
        if (getenv("USE_HWMON") && strcmp(getenv("USE_HWMON"), "1") == 0)
        {
            use_hwmon_interface = 1;
            int i = 0;
            int foundif = 0;
            while (1)
            {
                FILE *fp;
                char name[1024];
                sprintf(name, "/sys/class/hwmon/hwmon%d/name", i);
                fp = fopen(name, "rb");
                if (fp == 0)
                    break;
                fgets(name, 1023, fp);
                fclose(fp);
                if (strcmp(name, "clevo_xsm_wmi\n") == 0)
                {
                    printf("Found clevo interface %d %s", i, name);
                    hwmon_interface_num = i;
                    foundif = 1;
                    break;
                }
                i++;
            };
            if (!foundif)
                return EXIT_FAILURE;
        }
        autoset_cpu_gpu();
    }

    return EXIT_SUCCESS;
}

static void main_init_share(void)
{
    void *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
                     -1, 0);
    share_info = shm;
    share_info->exit = 0;
    share_info->cpu_temp = 0;
    share_info->gpu_temp = 0;
    share_info->cpu_fan_duty = 0;
    share_info->gpu_fan_duty = 0;
    share_info->cpu_fan_rpms = 0;
    share_info->gpu_fan_rpms = 0;
    share_info->auto_cpu_duty = 0;
    share_info->auto_gpu_duty = 0;
    share_info->auto_cpu_duty_val = 0;
    share_info->auto_gpu_duty_val = 0;
    share_info->manual_next_cpu_fan_duty = 0;
    share_info->manual_next_gpu_fan_duty = 0;
    share_info->manual_prev_cpu_fan_duty = 0;
    share_info->manual_prev_gpu_fan_duty = 0;
}

static EcTemperature g_gpu_temperature;
static pthread_mutex_t g_gpu_temperature_lock = PTHREAD_MUTEX_INITIALIZER;

static void *gpu_temp_smi_thread(void *arg)
{
    (void)arg;
    while (1) {
        int t = ec_query_gpu_temp_nvidia();
        pthread_mutex_lock(&g_gpu_temperature_lock);
        ec_temperature_update(&g_gpu_temperature, t, ec_monotonic_ms());
        pthread_mutex_unlock(&g_gpu_temperature_lock);
        sleep(1);
    }
    return NULL;
}

static int main_ec_worker(void)
{
    setuid(0);
    system("modprobe ec_sys");

    if (use_gpu_temp_smi) {
        pthread_t tid;
        if (pthread_create(&tid, NULL, gpu_temp_smi_thread, NULL) == 0)
            pthread_detach(tid);
        else
            fprintf(stderr, "GPU temperature thread unavailable; using EC\n");
    }

    int ec_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY | O_CLOEXEC);
    if (ec_fd < 0)
    {
        printf("unable to open EC sysfs: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    int initialized = 0;
    int prev_auto_cpu_duty = -1;
    int prev_auto_gpu_duty = -1;
    EcTemperature cpu_sensor = {0}, gpu_sensor = {0};
    EcFanCommand commands[2] = {0};
    int sensor_fault = 0;
    while (share_info->exit == 0)
    {
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1)
        {
            printf("worker on parent death\n");
            break;
        }
        EcSample sample;
        int sample_ok = ec_sample_read(ec_fd, pread, &sample) == 0;
        int64_t now = ec_monotonic_ms();
        if (sample_ok)
        {
            ec_temperature_update(&cpu_sensor, sample.cpu_temp, now);
            ec_temperature_update(&gpu_sensor, sample.gpu_temp, now);
            share_info->cpu_fan_duty = calculate_fan_duty(sample.cpu_duty);
            share_info->gpu_fan_duty = calculate_fan_duty(sample.gpu_duty);
            share_info->cpu_fan_rpms = calculate_fan_rpms(
                sample.rpm[0], sample.rpm[1]);
            share_info->gpu_fan_rpms = calculate_fan_rpms(
                sample.rpm[2], sample.rpm[3]);
            if (!initialized)
            {
                share_info->manual_prev_cpu_fan_duty = share_info->cpu_fan_duty;
                share_info->manual_prev_gpu_fan_duty = share_info->gpu_fan_duty;
                initialized = 1;
            }
        }
        share_info->cpu_temp = ec_temperature_value(&cpu_sensor, now);
        int gpu_temp = -1;
        if (use_gpu_temp_smi) {
            pthread_mutex_lock(&g_gpu_temperature_lock);
            gpu_temp = ec_temperature_value(&g_gpu_temperature, now);
            pthread_mutex_unlock(&g_gpu_temperature_lock);
        }
        share_info->gpu_temp = gpu_temp >= 0 ? gpu_temp :
            ec_temperature_value(&gpu_sensor, now);
        int fault = share_info->cpu_temp < 0 || share_info->gpu_temp < 0;
        if (fault != sensor_fault) {
            fprintf(stderr, "%s\n", fault ?
                "Temperature unavailable/stale: AUTO fans request 100%; manual settings retained" :
                "Temperature readings recovered");
            sensor_fault = fault;
            ec_reset_auto_duty_state(FAN_CPU);
            ec_reset_auto_duty_state(FAN_GPU);
        }
        if (share_info->auto_cpu_duty != prev_auto_cpu_duty)
        {
            ec_reset_auto_duty_state(FAN_CPU);
            prev_auto_cpu_duty = share_info->auto_cpu_duty;
            commands[0].target = 0;
        }
        if (share_info->auto_gpu_duty != prev_auto_gpu_duty)
        {
            ec_reset_auto_duty_state(FAN_GPU);
            prev_auto_gpu_duty = share_info->auto_gpu_duty;
            commands[1].target = 0;
        }
        // Auto mode uses the hotter of CPU/GPU because this Clevo's shared
        // heatsink/chassis means either hotspot raises the whole thermal load.
        int target_temp = fault ? AUTO_FAN_EMERGENCY_TEMP_C :
            MAX(share_info->cpu_temp, share_info->gpu_temp);
        for (int index = 0; index < 2; index++) {
            FanIndex fan = index == 0 ? FAN_CPU : FAN_GPU;
            int automatic = index == 0 ? share_info->auto_cpu_duty :
                                        share_info->auto_gpu_duty;
            int actual = sample_ok ? (index == 0 ? share_info->cpu_fan_duty :
                                                   share_info->gpu_fan_duty) : -1;
            int requested = index == 0 ? share_info->manual_next_cpu_fan_duty :
                                         share_info->manual_next_gpu_fan_duty;
            if (automatic) {
                int next = !fault && sample_ok ?
                    ec_auto_duty_adjust(fan, target_temp, actual) : 0;
                if (target_temp >= AUTO_FAN_EMERGENCY_TEMP_C)
                    next = 100;
                requested = next != 0 ? next : commands[index].target;
                /* Hold ordinary control on failed readback; never lower a fan
                   using a guessed duty. Emergency requests remain possible. */
                if (!sample_ok && requested < 100)
                    requested = 0;
            }
            if (ec_fan_command_due(&commands[index], requested, actual, now)) {
                int result = index == 0 ? ec_write_cpu_fan_duty(requested) :
                                          ec_write_gpu_fan_duty(requested);
                if (result != EXIT_SUCCESS)
                    fprintf(stderr, "%s fan write failed; retrying after 2 s\n",
                            index == 0 ? "CPU" : "GPU");
                else {
                    char s_time[256];
                    get_time_string(s_time, sizeof(s_time), "%m/%d %H:%M:%S");
                    printf("%s %s fan requested %d%%; awaiting readback\n",
                           s_time, index == 0 ? "CPU" : "GPU", requested);
                }
            }
            if (commands[index].confirmed) {
                if (index == 0) {
                    if (automatic)
                        share_info->auto_cpu_duty_val = actual;
                    else
                        share_info->manual_prev_cpu_fan_duty = actual;
                } else {
                    if (automatic)
                        share_info->auto_gpu_duty_val = actual;
                    else
                        share_info->manual_prev_gpu_fan_duty = actual;
                }
            }
        }
        //
        usleep(200 * 1000);
    }
    close(ec_fd);
    printf("worker quit\n");
    return EXIT_SUCCESS;
}

static const char *ui_setup_icon_theme(void)
{
    static const char *theme_dir = "/tmp/clevo-indicator-icons";
    char icon_path[256];

    mkdir("/tmp/clevo-indicator-icons", 0700);
    mkdir("/tmp/clevo-indicator-icons/hicolor", 0700);
    mkdir("/tmp/clevo-indicator-icons/hicolor/22x22", 0700);
    mkdir("/tmp/clevo-indicator-icons/hicolor/22x22/apps", 0700);

    /* GTK requires an index.theme in the root of the theme dir to recognise
       it as a valid icon theme; without it icon lookups silently fail. */
    snprintf(icon_path, sizeof(icon_path), "%s/hicolor/index.theme", theme_dir);
    FILE *idx = fopen(icon_path, "w");
    if (idx)
    {
        fprintf(idx,
                "[Icon Theme]\n"
                "Name=clevo-indicator\n"
                "Directories=22x22/apps\n"
                "\n"
                "[22x22/apps]\n"
                "Size=22\n"
                "Type=Fixed\n");
        fclose(idx);
    }

    GdkPixbuf *pb = ui_draw_fan_icon(22);
    if (pb)
    {
        snprintf(icon_path, sizeof(icon_path),
                 "%s/hicolor/22x22/apps/clevo-fan.png", theme_dir);
        gdk_pixbuf_save(pb, icon_path, "png", NULL, NULL);
        g_object_unref(pb);
    }

    /* 22×22 transparent icon — renders as nothing in the panel so only
       the label text (with embedded 🌀 emoji) is visible */
    cairo_surface_t *blank_surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 22, 22);
    cairo_t *blank_cr = cairo_create(blank_surf);
    cairo_set_source_rgba(blank_cr, 0, 0, 0, 0);
    cairo_paint(blank_cr);
    cairo_destroy(blank_cr);
    GdkPixbuf *blank_pb = gdk_pixbuf_get_from_surface(blank_surf, 0, 0, 22, 22);
    cairo_surface_destroy(blank_surf);
    if (blank_pb)
    {
        snprintf(icon_path, sizeof(icon_path),
                 "%s/hicolor/22x22/apps/clevo-blank.png", theme_dir);
        gdk_pixbuf_save(blank_pb, icon_path, "png", NULL, NULL);
        g_object_unref(blank_pb);
    }

    /* Use the Cairo-drawn fan icon for the CPU indicator */
    GdkPixbuf *cyclone_pb = ui_draw_fan_icon(22);
    if (cyclone_pb)
    {
        snprintf(icon_path, sizeof(icon_path),
                 "%s/hicolor/22x22/apps/clevo-cyclone.png", theme_dir);
        gdk_pixbuf_save(cyclone_pb, icon_path, "png", NULL, NULL);
        g_object_unref(cyclone_pb);
    }

    return theme_dir;
}

static void main_ui_worker(int argc, char **argv)
{
    gboolean force_legacy_appindicator = FALSE;

    printf("Indicator...\n");
    int desktop_uid = getuid();
    setuid(desktop_uid);
    gtk_init(&argc, &argv);

    force_legacy_appindicator = (g_getenv("CLEVO_LEGACY_APPINDICATOR") != NULL);

    if (!force_legacy_appindicator)
    {
        ClevoSniHandlers handlers = {
            .activate = ui_sni_activate,
            .context_menu = ui_sni_context_menu,
            .secondary_activate = ui_sni_secondary_activate,
        };
        experimental_sni = clevo_sni_new(&handlers, NULL);
        if (experimental_sni)
        {
            clevo_sni_set_title(experimental_sni, "Clevo Indicator");
            clevo_sni_set_status(experimental_sni, "Active");
            clevo_sni_set_show_icon(experimental_sni, FALSE);
            clevo_sni_set_prefer_activate(experimental_sni, TRUE);
            printf("native SNI enabled: %s%s\n",
                   clevo_sni_get_bus_name(experimental_sni),
                   clevo_sni_get_object_path(experimental_sni));
        }
        else
        {
            printf("native SNI init failed, falling back to legacy AppIndicator mode\n");
        }
    }
    else
    {
        printf("legacy AppIndicator mode forced via CLEVO_LEGACY_APPINDICATOR\n");
    }

    ui_use_experimental_sni = (experimental_sni != NULL);
    if (ui_use_experimental_sni)
    {
        ui_popup_init();
        g_timeout_add(500, &ui_update, NULL);
        ui_toggle_menuitems();
        gtk_main();
        printf("main on UI quit\n");
        return;
    }

    const char *fan_icon_theme = ui_setup_icon_theme();

    /* Panel stacks right-to-left: first created = rightmost.
       Create GPU first so it appears on the right, CPU second so it
       appears on the left. */

    /* ── AppIndicator 1 (rightmost): GPU fan control ── */
    GtkWidget *gpu_menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(gpu_menu), FALSE);
    for (int i = 0; i < control_rows_count; i++)
    {
        GtkWidget *item = gtk_menu_item_new_with_label(control_rows[i].label);
        g_signal_connect(item, "activate", G_CALLBACK(ui_fan_item_activated),
                         GINT_TO_POINTER(FAN_COMMAND(FAN_GPU, control_rows[i].duty)));
        gtk_menu_shell_append(GTK_MENU_SHELL(gpu_menu), item);
        control_rows[i].gpu_item = item;
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(gpu_menu), gtk_separator_menu_item_new());
    GtkWidget *gpu_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect_swapped(gpu_quit, "activate", G_CALLBACK(ui_command_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(gpu_menu), gpu_quit);
    gtk_widget_show_all(gpu_menu);

    /* 🌀 icon for CPU; blank (→ ...) for GPU; ° = \xc2\xb0 */
    indicator_gpu = app_indicator_new(NAME "-gpu", "clevo-blank",
                                      APP_INDICATOR_CATEGORY_HARDWARE);
    app_indicator_set_icon_theme_path(indicator_gpu, fan_icon_theme);
    app_indicator_set_status(indicator_gpu, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator_gpu, GTK_MENU(gpu_menu));
    app_indicator_set_label(indicator_gpu,
                            "G:--\xc2\xb0 --% \xf0\x9f\x8c\x80",
                            "G:000\xc2\xb0 100% \xf0\x9f\x8c\x80");

    /* ── AppIndicator 2 (leftmost): CPU fan control ── */
    GtkWidget *cpu_menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(cpu_menu), FALSE);
    for (int i = 0; i < control_rows_count; i++)
    {
        GtkWidget *item = gtk_menu_item_new_with_label(control_rows[i].label);
        g_signal_connect(item, "activate", G_CALLBACK(ui_fan_item_activated),
                         GINT_TO_POINTER(FAN_COMMAND(FAN_CPU, control_rows[i].duty)));
        gtk_menu_shell_append(GTK_MENU_SHELL(cpu_menu), item);
        control_rows[i].cpu_item = item;
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(cpu_menu), gtk_separator_menu_item_new());
    GtkWidget *cpu_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect_swapped(cpu_quit, "activate", G_CALLBACK(ui_command_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(cpu_menu), cpu_quit);
    gtk_widget_show_all(cpu_menu);

    indicator_cpu = app_indicator_new(NAME "-cpu", "clevo-blank",
                                      APP_INDICATOR_CATEGORY_HARDWARE);
    app_indicator_set_icon_theme_path(indicator_cpu, fan_icon_theme);
    app_indicator_set_status(indicator_cpu, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator_cpu, GTK_MENU(cpu_menu));
    app_indicator_set_label(indicator_cpu,
                            "\xf0\x9f\x8c\x80 C:--\xc2\xb0 --%",
                            "\xf0\x9f\x8c\x80 C:000\xc2\xb0 100%");

    g_signal_connect(indicator_cpu, "connection-changed",
                     G_CALLBACK(ui_on_reconnect), NULL);
    g_signal_connect(indicator_gpu, "connection-changed",
                     G_CALLBACK(ui_on_reconnect), NULL);
    g_timeout_add(500, &ui_update, NULL);
    ui_toggle_menuitems();
    gtk_main();
    printf("main on UI quit\n");
}

static void main_on_sigchld(int signum)
{
    (void)signum;
    /* A timed-out startup GPU query may exit after the worker is started.
       Only losing the actual EC worker should terminate the UI. */
    if (worker_pid > 0 && waitpid(worker_pid, NULL, WNOHANG) == worker_pid)
        _exit(EXIT_SUCCESS);
}

static void main_on_sigterm(int signum)
{
    printf("main on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
    exit(EXIT_SUCCESS);
}

static int main_dump_fan(void)
{
    printf("Dump fan information\n");
    printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
    printf("  CPU FAN Duty: %d%%\n", ec_query_cpu_fan_duty());
    printf("  CPU FAN RPMs: %d RPM\n", ec_query_cpu_fan_rpms());
    printf("  GPU Temp: %d°C\n", ec_query_gpu_temp());
    printf("  GPU FAN Duty: %d%%\n", ec_query_gpu_fan_duty());
    printf("  GPU RPMs: %d RPM\n", ec_query_gpu_fan_rpms());
    return EXIT_SUCCESS;
}

static int main_test_cpu_fan(int duty_percentage)
{
    printf("Change fan duty to %d%%\n", duty_percentage);
    ec_write_cpu_fan_duty(duty_percentage);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static int main_test_gpu_fan(int duty_percentage)
{
    printf("Change fan duty to %d%%\n", duty_percentage);
    ec_write_gpu_fan_duty(duty_percentage);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static gboolean ui_update(gpointer user_data)
{
    (void)user_data;

    int disp_cpu_duty = (share_info->manual_next_cpu_fan_duty != 0)
        ? share_info->manual_next_cpu_fan_duty : share_info->cpu_fan_duty;
    int disp_gpu_duty = (share_info->manual_next_gpu_fan_duty != 0)
        ? share_info->manual_next_gpu_fan_duty : share_info->gpu_fan_duty;

    char label[256];
    if (ui_use_experimental_sni)
    {
        snprintf(label, sizeof(label),
                 "\xf0\x9f\x8c\x80 C:%d\xc2\xb0 %d%%  G:%d\xc2\xb0 %d%% \xf0\x9f\x8c\x80",
                 share_info->cpu_temp, disp_cpu_duty,
                 share_info->gpu_temp, disp_gpu_duty);
    }
    else
    {
        snprintf(label, sizeof(label), "C:%d\xc2\xb0 %d%%  G:%d\xc2\xb0 %d%%",
                 share_info->cpu_temp, disp_cpu_duty,
                 share_info->gpu_temp, disp_gpu_duty);
    }

    if (strcmp(ui_last_label, label) != 0)
    {
        snprintf(ui_last_label, sizeof(ui_last_label), "%s", label);
        if (experimental_sni)
        {
            clevo_sni_set_label(experimental_sni,
                                label,
                                "\xf0\x9f\x8c\x80 C:000\xc2\xb0 100%  G:000\xc2\xb0 100% \xf0\x9f\x8c\x80");
        }
        if (indicator_cpu)
        {
            char cpu_lbl[32];
            snprintf(cpu_lbl, sizeof(cpu_lbl), "\xf0\x9f\x8c\x80 C:%d\xc2\xb0 %d%%",
                     share_info->cpu_temp, disp_cpu_duty);
            app_indicator_set_label(indicator_cpu, cpu_lbl, "\xf0\x9f\x8c\x80 C:000\xc2\xb0 100%");
        }
        if (indicator_gpu)
        {
            char gpu_lbl[32];
            snprintf(gpu_lbl, sizeof(gpu_lbl), "G:%d\xc2\xb0 %d%% \xf0\x9f\x8c\x80",
                     share_info->gpu_temp, disp_gpu_duty);
            app_indicator_set_label(indicator_gpu, gpu_lbl,
                                    "G:000\xc2\xb0 100% \xf0\x9f\x8c\x80");
        }
    }

    ui_toggle_menuitems();
    return G_SOURCE_CONTINUE;
}

/* Called when the panel notifier host connects or disconnects.
   On reconnect we clear the cached label so ui_update pushes it again. */
static void ui_on_reconnect(AppIndicator *ind, gboolean connected, gpointer data)
{
    (void)ind; (void)data;
    if (connected)
        ui_last_label[0] = '\0';
}

static void ui_command_set_fan(long fan_duty)
{
    int fan_index = FAN_CMD_FAN(fan_duty);
    int fan_duty_val = FAN_CMD_DUTY(fan_duty);
    if (fan_index != FAN_CPU && fan_index != FAN_GPU)
        return;
    const char *fan_name = (fan_index == FAN_CPU) ? "CPU" : "GPU";

    if (fan_duty_val == 0)
    {
        printf("clicked on %s fan duty auto\n", fan_name);
        if (fan_index == FAN_CPU)
        {
            share_info->auto_cpu_duty = 1;
            share_info->auto_cpu_duty_val = 0;
            share_info->manual_next_cpu_fan_duty = 0;
        }
        else
        {
            share_info->auto_gpu_duty = 1;
            share_info->auto_gpu_duty_val = 0;
            share_info->manual_next_gpu_fan_duty = 0;
        }
    }
    else
    {
        printf("clicked on %s fan duty: %d\n", fan_name, fan_duty_val);
        if (fan_index == FAN_CPU)
        {
            share_info->auto_cpu_duty = 0;
            share_info->auto_cpu_duty_val = 0;
            share_info->manual_next_cpu_fan_duty = fan_duty_val;
        }
        else
        {
            share_info->auto_gpu_duty = 0;
            share_info->auto_gpu_duty_val = 0;
            share_info->manual_next_gpu_fan_duty = fan_duty_val;
        }
    }
    ui_toggle_menuitems();
}

static void ui_fan_item_activated(GtkMenuItem *item, gpointer data)
{
    (void)item;
    ui_command_set_fan((long)GPOINTER_TO_INT(data));
}

static void ui_fan_btn_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    ui_popup_hide();
    ui_command_set_fan((long)GPOINTER_TO_INT(data));
}

static void ui_command_quit(gchar *command)
{
    (void)command;
    printf("clicked on quit\n");
    if (experimental_sni)
    {
        clevo_sni_free(experimental_sni);
        experimental_sni = NULL;
    }
    gtk_main_quit();
}

static const char *ui_popup_row_class(int duty, MenuItemType type)
{
    if (type == AUTO)
        return "fan-auto";

    switch (duty)
    {
    case 40:  return "fan-duty-40";
    case 50:  return "fan-duty-50";
    case 60:  return "fan-duty-60";
    case 70:  return "fan-duty-70";
    case 80:  return "fan-duty-80";
    case 90:  return "fan-duty-90";
    case 100: return "fan-duty-100";
    default:  return "fan-auto";
    }
}

static void ui_popup_init(void)
{
    if (fan_popup_window)
        return;

    fan_popup_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(fan_popup_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(fan_popup_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(fan_popup_window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(fan_popup_window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(fan_popup_window), TRUE);
    gtk_window_set_focus_on_map(GTK_WINDOW(fan_popup_window), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(fan_popup_window),
                             GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_resizable(GTK_WINDOW(fan_popup_window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(fan_popup_window), 0);
    g_signal_connect(fan_popup_window, "focus-out-event",
                     G_CALLBACK(ui_popup_focus_out), NULL);
    g_signal_connect(fan_popup_window, "key-press-event",
                     G_CALLBACK(ui_popup_key_press), NULL);

    GtkCssProvider *popup_css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(
        popup_css,
        "window.clevo-popup button.fan-choice {"
        "  background-image: none;"
        "  color: #3c4149;"
        "  border-radius: 0;"
        "  border: 1px solid transparent;"
        "  box-shadow: none;"
        "  padding: 6px 10px;"
        "  margin: 0;"
        "}"
        "window.clevo-popup button.fan-choice.fan-auto {"
        "  background-color: #f4f6f8;"
        "}"
        "window.clevo-popup button.fan-choice.fan-duty-40 {"
        "  background-color: #e4f4de;"
        "}"
        "window.clevo-popup button.fan-choice.fan-duty-50 {"
        "  background-color: #ebf4d7;"
        "}"
        "window.clevo-popup button.fan-choice.fan-duty-60 {"
        "  background-color: #f2f0cf;"
        "}"
        "window.clevo-popup button.fan-choice.fan-duty-70 {"
        "  background-color: #f7e7c8;"
        "}"
        "window.clevo-popup button.fan-choice.fan-duty-80 {"
        "  background-color: #f7d7bc;"
        "}"
        "window.clevo-popup button.fan-choice.fan-duty-90 {"
        "  background-color: #f4c4b5;"
        "}"
        "window.clevo-popup button.fan-choice.fan-duty-100 {"
        "  background-color: #efb2b2;"
        "}"
        "window.clevo-popup button.fan-choice:hover {"
        "  border-color: rgba(69, 83, 108, 0.22);"
        "  box-shadow: inset 0 0 0 1px rgba(69, 83, 108, 0.12);"
        "}"
        "window.clevo-popup button.fan-choice.fan-selected {"
        "  color: #24303d;"
        "}"
        "window.clevo-popup button.fan-choice label {"
        "  font-weight: 500;"
        "}"
        "window.clevo-popup button.fan-choice.fan-selected label {"
        "  font-weight: 700;"
        "}",
        -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(popup_css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(popup_css);
    gtk_style_context_add_class(gtk_widget_get_style_context(fan_popup_window),
                                "clevo-popup");

    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_ETCHED_OUT);
    gtk_container_add(GTK_CONTAINER(fan_popup_window), frame);

    GtkWidget *outer_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(outer_box), 6);
    gtk_container_add(GTK_CONTAINER(frame), outer_box);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 0);
    gtk_box_pack_start(GTK_BOX(outer_box), grid, TRUE, TRUE, 0);

    GtkWidget *cpu_hdr = gtk_label_new(NULL);
    GtkWidget *gpu_hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(cpu_hdr), "<b>CPU</b>");
    gtk_label_set_markup(GTK_LABEL(gpu_hdr), "<b>GPU</b>");
    gtk_widget_set_hexpand(cpu_hdr, TRUE);
    gtk_widget_set_hexpand(gpu_hdr, TRUE);
    gtk_grid_attach(GTK_GRID(grid), cpu_hdr, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
                    gtk_separator_new(GTK_ORIENTATION_VERTICAL),
                    1, 0, 1, control_rows_count + 2);
    gtk_grid_attach(GTK_GRID(grid), gpu_hdr, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid),
                    gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                    0, 1, 3, 1);

    for (int i = 0; i < control_rows_count && i < MAX_FAN_ROWS; i++)
    {
        GtkWidget *cpu_btn = gtk_button_new_with_label(control_rows[i].label);
        GtkWidget *gpu_btn = gtk_button_new_with_label(control_rows[i].label);

        gtk_button_set_relief(GTK_BUTTON(cpu_btn), GTK_RELIEF_NORMAL);
        gtk_button_set_relief(GTK_BUTTON(gpu_btn), GTK_RELIEF_NORMAL);
        gtk_widget_set_hexpand(cpu_btn, TRUE);
        gtk_widget_set_hexpand(gpu_btn, TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(cpu_btn),
                                    "fan-choice");
        gtk_style_context_add_class(gtk_widget_get_style_context(gpu_btn),
                                    "fan-choice");
        gtk_style_context_add_class(gtk_widget_get_style_context(cpu_btn),
                                    ui_popup_row_class(control_rows[i].duty,
                                                       control_rows[i].type));
        gtk_style_context_add_class(gtk_widget_get_style_context(gpu_btn),
                                    ui_popup_row_class(control_rows[i].duty,
                                                       control_rows[i].type));

        g_signal_connect(cpu_btn, "clicked", G_CALLBACK(ui_fan_btn_clicked),
                         GINT_TO_POINTER(FAN_COMMAND(FAN_CPU, control_rows[i].duty)));
        g_signal_connect(gpu_btn, "clicked", G_CALLBACK(ui_fan_btn_clicked),
                         GINT_TO_POINTER(FAN_COMMAND(FAN_GPU, control_rows[i].duty)));

        gtk_grid_attach(GTK_GRID(grid), cpu_btn, 0, i + 2, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), gpu_btn, 2, i + 2, 1, 1);

        control_rows[i].cpu_btn = cpu_btn;
        control_rows[i].gpu_btn = gpu_btn;
    }

    gtk_box_pack_start(GTK_BOX(outer_box),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *quit_btn = gtk_button_new_with_label("Quit");
    gtk_widget_set_size_request(quit_btn, -1, 30);
    g_signal_connect_swapped(quit_btn, "clicked", G_CALLBACK(ui_command_quit), NULL);
    gtk_box_pack_start(GTK_BOX(outer_box), quit_btn, FALSE, FALSE, 0);
}

static void ui_popup_show_at(int x, int y)
{
    char *startup_token = NULL;

    if (!fan_popup_window)
        return;

    if (experimental_sni)
        startup_token = clevo_sni_take_activation_token(experimental_sni);

    if (gtk_widget_is_visible(fan_popup_window))
    {
        ui_popup_hide();
        if (startup_token && startup_token[0] != '\0')
            gdk_notify_startup_complete_with_id(startup_token);
        g_free(startup_token);
        return;
    }

    {
        GdkDisplay *display = gdk_display_get_default();
        GdkSeat *seat = display ? gdk_display_get_default_seat(display) : NULL;
        GdkDevice *pointer = seat ? gdk_seat_get_pointer(seat) : NULL;
        if (pointer)
            gdk_device_get_position(pointer, NULL, &x, &y);
        else if (x < 0 || y < 0)
        {
            x = 0;
            y = 0;
        }
    }

    GtkRequisition natural = {0};
    gtk_widget_get_preferred_size(fan_popup_window, NULL, &natural);
    int width = natural.width > 0 ? natural.width : 320;
    int height = natural.height > 0 ? natural.height : 320;
    int px = x - (width / 2);
    int py = y + 8;

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = NULL;
    if (display)
        monitor = gdk_display_get_monitor_at_point(display, x, y);
    if (!monitor && display)
        monitor = gdk_display_get_primary_monitor(display);
    if (monitor)
    {
        GdkRectangle workarea;
        gdk_monitor_get_workarea(monitor, &workarea);
        if (px < workarea.x)
            px = workarea.x;
        if (py < workarea.y)
            py = workarea.y;
        if (px + width > workarea.x + workarea.width)
            px = ((workarea.width > width)
                ? (workarea.x + workarea.width - width)
                : workarea.x);
        if (py + height > workarea.y + workarea.height)
            py = ((workarea.height > height)
                ? (workarea.y + workarea.height - height)
                : workarea.y);
    }

    ui_toggle_menuitems();
    if (startup_token && startup_token[0] != '\0')
        gtk_window_set_startup_id(GTK_WINDOW(fan_popup_window), startup_token);
    gtk_window_move(GTK_WINDOW(fan_popup_window), px, py);
    gtk_widget_show_all(fan_popup_window);
    gtk_window_present_with_time(GTK_WINDOW(fan_popup_window), GDK_CURRENT_TIME);

    GdkWindow *gdk_win = gtk_widget_get_window(fan_popup_window);
    if (gdk_win)
        gdk_window_move(gdk_win, px, py);

    if (control_rows_count > 0 && control_rows[0].cpu_btn)
        gtk_widget_grab_focus(control_rows[0].cpu_btn);

    if (startup_token && startup_token[0] != '\0')
    {
        gdk_notify_startup_complete_with_id(startup_token);
        g_free(startup_token);
    }
    else
    {
        g_free(startup_token);
    }
}

static void ui_popup_hide(void)
{
    if (fan_popup_window)
        gtk_widget_hide(fan_popup_window);
}

static gboolean ui_popup_focus_out(GtkWidget *widget, GdkEventFocus *event,
                                   gpointer user_data)
{
    (void)event;
    (void)user_data;

    if (widget)
    {
        GdkDisplay *display = gdk_display_get_default();
        GdkSeat *seat = display ? gdk_display_get_default_seat(display) : NULL;
        GdkDevice *pointer = seat ? gdk_seat_get_pointer(seat) : NULL;
        GdkWindow *window = gtk_widget_get_window(widget);
        GdkModifierType mask = 0;

        if (pointer && window)
            gdk_device_get_state(pointer, window, NULL, &mask);

        if (mask & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK))
            ui_popup_hide();
    }

    return FALSE;
}

static gboolean ui_popup_key_press(GtkWidget *widget, GdkEventKey *event,
                                   gpointer user_data)
{
    (void)widget;
    (void)user_data;

    if (event->keyval == GDK_KEY_Escape)
    {
        ui_popup_hide();
        return TRUE;
    }

    return FALSE;
}

static void ui_sni_activate(int x, int y, void *user_data)
{
    (void)user_data;
    ui_popup_show_at(x, y);
}

static void ui_sni_context_menu(int x, int y, void *user_data)
{
    (void)user_data;
    ui_popup_show_at(x, y);
}

static void ui_sni_secondary_activate(int x, int y, void *user_data)
{
    (void)user_data;
    ui_popup_show_at(x, y);
}

/* Draw a 3-blade propeller fan icon into a size×size ARGB pixbuf. */
static GdkPixbuf *ui_draw_fan_icon(int size)
{
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surf);

    double cx  = size / 2.0;
    double cy  = size / 2.0;
    double r   = size / 2.0 - 1.0;   /* outer radius */
    double hub = size / 7.0;          /* hub radius */
    int blades = 3;

    /* transparent background */
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);

    /* blades */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.92);
    for (int i = 0; i < blades; i++)
    {
        double base = (2.0 * M_PI * i) / blades - M_PI / 2.0;
        /* outer arc from base to base+110°, inner arc back narrower */
        cairo_move_to(cr, cx + hub * cos(base), cy + hub * sin(base));
        cairo_arc(cr, cx, cy, r,   base,              base + 1.9);
        cairo_arc_negative(cr, cx, cy, hub, base + 1.9, base + 0.5);
        cairo_close_path(cr);
        cairo_fill(cr);
    }

    /* hub */
    cairo_arc(cr, cx, cy, hub * 0.75, 0, 2.0 * M_PI);
    cairo_fill(cr);

    cairo_destroy(cr);
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surf, 0, 0, size, size);
    cairo_surface_destroy(surf);
    return pixbuf;
}

static void ui_toggle_menuitems(void)
{
    int is_cpu_auto  = share_info->auto_cpu_duty;
    int is_gpu_auto  = share_info->auto_gpu_duty;
    int pending_cpu  = share_info->manual_next_cpu_fan_duty;
    int pending_gpu  = share_info->manual_next_gpu_fan_duty;
    int cpu_sel_duty = is_cpu_auto ? 0
        : (pending_cpu  != 0 ? pending_cpu  : share_info->manual_prev_cpu_fan_duty);
    int gpu_sel_duty = is_gpu_auto ? 0
        : (pending_gpu != 0 ? pending_gpu : share_info->manual_prev_gpu_fan_duty);

    for (int i = 0; i < control_rows_count && i < MAX_FAN_ROWS; i++)
    {
        int sel_cpu = (control_rows[i].type == AUTO)
            ? is_cpu_auto
            : (!is_cpu_auto && control_rows[i].duty == cpu_sel_duty);
        int sel_gpu = (control_rows[i].type == AUTO)
            ? is_gpu_auto
            : (!is_gpu_auto && control_rows[i].duty == gpu_sel_duty);

        char cpu_text[64], gpu_text[64];
        snprintf(cpu_text, sizeof(cpu_text), "%s %s",
                 sel_cpu ? "\xe2\x80\xa2" : " ", control_rows[i].label);
        snprintf(gpu_text, sizeof(gpu_text), "%s %s",
                 sel_gpu ? "\xe2\x80\xa2" : " ", control_rows[i].label);

        if (control_rows[i].cpu_item)
            gtk_menu_item_set_label(GTK_MENU_ITEM(control_rows[i].cpu_item), cpu_text);
        if (control_rows[i].gpu_item)
            gtk_menu_item_set_label(GTK_MENU_ITEM(control_rows[i].gpu_item), gpu_text);
        if (control_rows[i].cpu_btn)
        {
            gtk_button_set_label(GTK_BUTTON(control_rows[i].cpu_btn), cpu_text);
            if (sel_cpu)
                gtk_style_context_add_class(
                    gtk_widget_get_style_context(control_rows[i].cpu_btn),
                    "fan-selected");
            else
                gtk_style_context_remove_class(
                    gtk_widget_get_style_context(control_rows[i].cpu_btn),
                    "fan-selected");
        }
        if (control_rows[i].gpu_btn)
        {
            gtk_button_set_label(GTK_BUTTON(control_rows[i].gpu_btn), gpu_text);
            if (sel_gpu)
                gtk_style_context_add_class(
                    gtk_widget_get_style_context(control_rows[i].gpu_btn),
                    "fan-selected");
            else
                gtk_style_context_remove_class(
                    gtk_widget_get_style_context(control_rows[i].gpu_btn),
                    "fan-selected");
        }
    }
}

static int ec_init(void)
{
    if (ioperm(EC_DATA, 1, 1) != 0)
        return EXIT_FAILURE;
    if (ioperm(EC_SC, 1, 1) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum)
{
    printf("ec on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
}

/* Interpolate between neighboring anchor points in auto_fan_curve[]. */
static int ec_auto_curve_target_duty(int temp)
{
    if (temp <= auto_fan_curve[0].temp)
        return auto_fan_curve[0].duty;

    for (int i = 1; i < auto_fan_curve_count; i++)
    {
        AutoFanCurvePoint low = auto_fan_curve[i - 1];
        AutoFanCurvePoint high = auto_fan_curve[i];
        if (temp <= high.temp)
        {
            int temp_span = high.temp - low.temp;
            int duty_span = high.duty - low.duty;
            return low.duty +
                   (temp - low.temp) * duty_span / temp_span;
        }
    }

    return auto_fan_curve[auto_fan_curve_count - 1].duty;
}

static long ec_elapsed_ms_since(const struct timespec *start,
                                const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000L +
           (end->tv_nsec - start->tv_nsec) / 1000000L;
}

static AutoFanState *ec_auto_fan_state(FanIndex fan)
{
    switch (fan)
    {
    case FAN_CPU:
        return &auto_fan_states[0];
    case FAN_GPU:
        return &auto_fan_states[1];
    default:
        return NULL;
    }
}

/* Clear per-fan smoothing state when the worker sees an auto/manual mode flip. */
static void ec_reset_auto_duty_state(FanIndex fan)
{
    AutoFanState *state = ec_auto_fan_state(fan);
    if (state == NULL)
        return;

    memset(state, 0, sizeof(*state));
}

/* Auto control applies the shared curve against the live EC duty, with
   hysteresis/rate limiting for normal adjustments but immediate correction on
   first entry to AUTO, emergency temperatures, or controller re-sync. */
static int ec_auto_duty_adjust(FanIndex fan, int temp, int current_duty)
{
    AutoFanState *state = ec_auto_fan_state(fan);
    if (state == NULL)
        return 0;

    int first_adjustment = !state->initialized;
    if (!state->initialized)
    {
        state->initialized = 1;
        state->last_applied_duty = current_duty;
        state->last_temp = temp;
        clock_gettime(CLOCK_MONOTONIC, &state->last_change_at);
    }

    if (temp >= AUTO_FAN_EMERGENCY_TEMP_C)
    {
        if (current_duty >= 100)
            return 0;
        state->last_applied_duty = 100;
        state->last_temp = temp;
        clock_gettime(CLOCK_MONOTONIC, &state->last_change_at);
        return 100;
    }

    int target_duty = ec_auto_curve_target_duty(temp);
    int current_auto_duty = current_duty;
    int duty_delta = target_duty - current_auto_duty;
    int controller_desynced =
        abs(current_duty - state->last_applied_duty) >= AUTO_FAN_MIN_DUTY_DELTA;
    if (abs(duty_delta) < AUTO_FAN_MIN_DUTY_DELTA)
    {
        state->last_applied_duty = current_duty;
        return 0;
    }

    if (!first_adjustment && !controller_desynced)
    {
        /* Temperature hysteresis keeps the controller from chasing 1-2C noise
           when the fan is already near the requested operating point. */
        if (duty_delta > 0 && temp < state->last_temp + AUTO_FAN_UP_HYSTERESIS_C)
            return 0;
        if (duty_delta < 0 && temp > state->last_temp - AUTO_FAN_DOWN_HYSTERESIS_C)
            return 0;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = ec_elapsed_ms_since(&state->last_change_at, &now);
    if (!first_adjustment && !controller_desynced &&
        elapsed_ms < AUTO_FAN_MIN_ADJUST_MS && abs(duty_delta) < 10)
        return 0;

    state->last_applied_duty = target_duty;
    state->last_temp = temp;
    state->last_change_at = now;
    return target_duty;
}

static int ec_query_cpu_temp(void)
{
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/temp1_input", hwmon_interface_num);
        FILE *fp;
        fp = fopen(name, "rb");
        if (fp == 0)
            return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return (val / 1000);
    }
    return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void)
{
    if (use_gpu_temp_smi)
    {
        int smi_temp = ec_query_gpu_temp_nvidia();
        if (smi_temp > 0)
            return smi_temp;
    }
    return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_gpu_temp_nvidia(void)
{
    static EcGpuQuery query = {0};
    static pid_t owner = 0;
    if (owner != getpid()) {
        /* A forked worker does not own its parent's outstanding child. */
        query.child = 0;
        owner = getpid();
    }
    return ec_gpu_query(&query, "/usr/bin/nvidia-smi", 1500);
}

static int ec_query_cpu_fan_duty(void)
{
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm1", hwmon_interface_num);
        FILE *fp;
        fp = fopen(name, "rb");
        if (fp == 0)
            return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return calculate_fan_duty(val);
    }
    int raw_duty = ec_io_read(EC_REG_CPU_FAN_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_cpu_fan_rpms(void)
{
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/fan1_input", hwmon_interface_num);
        FILE *fp;
        fp = fopen(name, "rb");
        if (fp == 0)
            return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return val;
    }
    int raw_rpm_hi = ec_io_read(EC_REG_CPU_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_CPU_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_query_gpu_fan_duty(void)
{
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm2", hwmon_interface_num);
        FILE *fp;
        fp = fopen(name, "rb");
        if (fp == 0)
            return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return calculate_fan_duty(val);
    }
    int raw_duty = ec_io_read(EC_REG_GPU_FAN_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_gpu_fan_rpms(void)
{
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/fan2_input", hwmon_interface_num);
        FILE *fp;
        fp = fopen(name, "rb");
        if (fp == 0)
            return 99;
        fgets(name, 1023, fp);
        fclose(fp);
        int val;
        sscanf(name, "%d\n", &val);
        return val;
    }
    int raw_rpm_hi = ec_io_read(EC_REG_GPU_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_GPU_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_write_cpu_fan_duty(int duty_percentage)
{
    if (duty_percentage < 0 || duty_percentage > 100)
    {
        printf("Wrong fan duty to write: %d\n", duty_percentage);
        return EXIT_FAILURE;
    }
    double v_d = ((double)duty_percentage) / 100.0 * 255.0 + 0.5;
    int v_i = (int)v_d;
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm1", hwmon_interface_num);
        FILE *fp;
        fp = fopen(name, "wb");
        if (fp == 0)
            return 99;
        int written = fprintf(fp, "%d\n", v_i);
        int closed = fclose(fp);
        return written < 0 || closed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    return ec_io_do(0x99, 0x01, v_i);
}

static int ec_write_gpu_fan_duty(int duty_percentage)
{
    if (duty_percentage < 0 || duty_percentage > 100)
    {
        printf("Wrong fan duty to write: %d\n", duty_percentage);
        return EXIT_FAILURE;
    }
    double v_d = ((double)duty_percentage) / 100.0 * 255.0 + 0.5;
    int v_i = (int)v_d;
    if (use_hwmon_interface)
    {
        char name[1024];
        sprintf(name, "/sys/class/hwmon/hwmon%d/pwm2", hwmon_interface_num);
        FILE *fp;
        fp = fopen(name, "wb");
        if (fp == 0)
            return 99;
        int written = fprintf(fp, "%d\n", v_i);
        int closed = fclose(fp);
        return written < 0 || closed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    return ec_io_do(0x99, 0x02, v_i);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
                      const char value)
{
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100))
    {
        usleep(1000);
        data = inb(port);
    }
    if (((data >> flag) & 0x1) != value)
    {
        printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
               port, data, flag, value);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int ec_io_read(const uint32_t port)
{
    if (ec_io_wait(EC_SC, IBF, 0) != EXIT_SUCCESS)
        return -1;
    outb(EC_SC_READ_CMD, EC_SC);

    if (ec_io_wait(EC_SC, IBF, 0) != EXIT_SUCCESS)
        return -1;
    outb(port, EC_DATA);

    //wait_ec(EC_SC, EC_SC_IBF_FREE);
    if (ec_io_wait(EC_SC, OBF, 1) != EXIT_SUCCESS)
        return -1;
    uint8_t value = inb(EC_DATA);

    return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
                    const uint8_t value)
{
    if (ec_io_wait(EC_SC, IBF, 0) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    outb(cmd, EC_SC);

    if (ec_io_wait(EC_SC, IBF, 0) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    outb(port, EC_DATA);

    if (ec_io_wait(EC_SC, IBF, 0) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int calculate_fan_duty(int raw_duty)
{
    if (raw_duty < 0)
        return -1;
    return (int)((double)raw_duty / 255.0 * 100.0 + 0.5);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low)
{
    if (raw_rpm_high < 0 || raw_rpm_low < 0)
        return -1;
    int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
    return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int check_proc_instances(const char *proc_name)
{
    int proc_name_len = strlen(proc_name);
    pid_t this_pid = getpid();
    DIR *dir;
    if (!(dir = opendir("/proc")))
    {
        perror("can't open /proc");
        return -1;
    }
    int instance_count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        char *endptr;
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0')
            continue;
        if (lpid == this_pid)
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "/proc/%ld/comm", lpid);
        FILE *fp = fopen(buf, "r");
        if (fp)
        {
            if (fgets(buf, sizeof(buf), fp) != NULL)
            {
                if ((buf[proc_name_len] == '\n' || buf[proc_name_len] == '\0') && strncmp(buf, proc_name, proc_name_len) == 0)
                {
                    fprintf(stderr, "Process: %ld\n", lpid);
                    instance_count += 1;
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return instance_count;
}

static void get_time_string(char *buffer, size_t max, const char *format)
{
    time_t timer;
    struct tm tm_info;
    time(&timer);
    localtime_r(&timer, &tm_info);
    strftime(buffer, max, format, &tm_info);
}

static void signal_term(__sighandler_t handler)
{
    signal(SIGHUP, handler);
    signal(SIGINT, handler);
    signal(SIGQUIT, handler);
    signal(SIGPIPE, handler);
    signal(SIGALRM, handler);
    signal(SIGTERM, handler);
    signal(SIGUSR1, handler);
    signal(SIGUSR2, handler);
}
