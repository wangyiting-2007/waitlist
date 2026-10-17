

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#endif

/* ---------------------------------------------------------------------------
 *  控制台输出编码开关：Windows 默认按 GBK 处理；定义 UTF8_TERMINAL 走 UTF-8。
 * ------------------------------------------------------------------------- */
#if defined(_WIN32) && !defined(UTF8_TERMINAL)
#  define CONSOLE_GBK 1
#else
#  define CONSOLE_GBK 0
#endif

/* ============================== 常量定义 ================================= */

#define MAX_NAME    32
#define MAX_ID      24
#define MAX_ORDER   16

/* ============================== 数据结构 ================================= */

/* 一条候补订单：题目要求仅含「订单号、旅客姓名、身份证号」三项 */
typedef struct {
    char orderNo[MAX_ORDER];    /* 订单号（系统自动生成） */
    char name[MAX_NAME];        /* 旅客姓名               */
    char idCard[MAX_ID];        /* 身份证号               */
    int  ticketCount;           /* 本笔订单所需票数（基础版每单 1 张） */
} TicketOrder;

/* 链式队列结点 */
typedef struct QNode {
    TicketOrder   data;
    struct QNode *next;
} QNode;

/* 带头结点的链式队列 */
typedef struct {
    QNode *front;   /* 队头指针，指向哑结点            */
    QNode *rear;    /* 队尾指针；空队列时 == front     */
    int    size;    /* 当前等待中的订单笔数（O(1) 求长）*/
} LinkQueue;

/* ============================== 全局状态 ================================= */

static LinkQueue g_queue;      /* 候补队列，队头即最早候补、优先兑现 */
static int       g_pool = 0;   /* 当前可用余票张数                   */
static long      g_seq  = 0;   /* 订单号流水号（保证唯一）           */

/* ========================= 一、控制台与输出辅助 ========================== */

/*
 * 初始化控制台编码。
 *   GBK 模式 ：把控制台代码页设为 936，与本程序 GBK 字节一致，中文不乱码。
 *   UTF-8 模式：设为 65001。
 */
static void initConsole(void)
{
#ifdef _WIN32
#  if CONSOLE_GBK
    SetConsoleOutputCP(936);
    SetConsoleCP(936);
#  else
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#  endif
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
}

/*
 * 计算字符串在终端上的显示宽度（中文占 2 列），用于表格对齐。
 *   GBK：汉字 2 字节 2 列；UTF-8：汉字 3 字节 2 列。
 */
#if CONSOLE_GBK
static int displayWidth(const char *s)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80) { w += 1; p += 1; }
        else           { w += 2; p += 2; }
    }
    return w;
}
#else
static int displayWidth(const char *s)
{
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p < 0x80)           { w += 1; p += 1; }
        else if ((*p >> 5) == 6){ w += 2; p += 2; }
        else if ((*p >> 4) == 14){w += 2; p += 3; }
        else                    { w += 2; p += 4; }
    }
    return w;
}
#endif

/* 按显示宽度左对齐输出，不足补空格（解决中文表格参差不齐） */
static void padPrint(const char *s, int width)
{
    int w = displayWidth(s);
    fputs(s, stdout);
    for (int i = w; i < width; i++) putchar(' ');
}

/* 读取一行（去结尾 \r\n） */
static void readLine(char *buf, int size)
{
    if (fgets(buf, size, stdin) == NULL) { buf[0] = '\0'; return; }
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
}

/* 读取 [lo, hi] 整数，非法输入循环提示 */
static int readIntRange(const char *prompt, int lo, int hi)
{
    char buf[64];
    for (;;) {
        printf("%s", prompt);
        readLine(buf, sizeof(buf));
        char *end = NULL;
        long v = strtol(buf, &end, 10);
        if (end != NULL) {
            while (*end == ' ' || *end == '\t') end++;
            if (end != buf && *end == '\0' && v >= lo && v <= hi) return (int)v;
        }
        printf("  [输入无效] 请输入 %d ~ %d 之间的整数。\n", lo, hi);
    }
}

/* 读取非空文本 */
static void readText(const char *prompt, char *buf, int size)
{
    for (;;) {
        printf("%s", prompt);
        readLine(buf, size);
        int blank = 1;
        for (char *p = buf; *p; p++)
            if (!isspace((unsigned char)*p)) { blank = 0; break; }
        if (!blank) return;
        printf("  [输入无效] 内容不能为空。\n");
    }
}

/* 生成唯一订单号：HB + YYMMDD + 4 位流水号，如 HB2610150001 */
static void generateOrderNo(char *out, int size)
{
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    g_seq++;
    unsigned yy = (unsigned)((tmv->tm_year + 1900) % 100);
    unsigned mm = (unsigned)(tmv->tm_mon + 1);
    unsigned dd = (unsigned)(tmv->tm_mday);
    unsigned sq = (unsigned)((g_seq % 10000 + 10000) % 10000);
    snprintf(out, size, "HB%02u%02u%02u%04u", yy, mm, dd, sq);
}

/* ========================= 二、链式队列基本操作 ========================== */

static int queueInit(LinkQueue *q)
{
    q->front = (QNode *)malloc(sizeof(QNode));
    if (q->front == NULL) { q->rear = NULL; q->size = 0; return 0; }
    q->front->next = NULL;
    q->rear  = q->front;
    q->size  = 0;
    return 1;
}

static int queueEmpty(const LinkQueue *q) { return q->front == q->rear; }

/* 入队：O(1)，尾插 */
static int queueEnqueue(LinkQueue *q, const TicketOrder *o)
{
    QNode *p = (QNode *)malloc(sizeof(QNode));
    if (p == NULL) return 0;
    p->data = *o;
    p->next = NULL;
    q->rear->next = p;
    q->rear = p;
    q->size++;
    return 1;
}

/* 出队：O(1)，摘队头 */
static int queueDequeue(LinkQueue *q, TicketOrder *out)
{
    if (queueEmpty(q)) return 0;
    QNode *p = q->front->next;
    if (out) *out = p->data;
    q->front->next = p->next;
    if (q->rear == p) q->rear = q->front;
    free(p);
    q->size--;
    return 1;
}

static void queueDestroy(LinkQueue *q)
{
    QNode *p = q->front;
    while (p != NULL) { QNode *nxt = p->next; free(p); p = nxt; }
    q->front = q->rear = NULL;
    q->size = 0;
}

/* ========================= 三、界面输出（表格） ========================== */

static void printSectionTitle(const char *title)
{
    printf("\n");
    printf("--------------------------------------------------------------\n");
    printf("  %s\n", title);
    printf("--------------------------------------------------------------\n");
}

/* 打印单条订单行（基础版：排队序号 / 订单号 / 姓名 / 身份证号） */
static void printOrderRow(int index, const TicketOrder *o)
{
    char idxBuf[16];
    if (index > 0) snprintf(idxBuf, sizeof(idxBuf), "%d", index);
    else            strcpy(idxBuf, "-");

    printf("| ");
    padPrint(idxBuf, 4);     printf(" | ");
    padPrint(o->orderNo, 14); printf(" | ");
    padPrint(o->name, 10);    printf(" | ");
    padPrint(o->idCard, 20);  printf(" |\n");
}

static void printTableHead(void)
{
    printf("+------+----------------+------------+----------------------+\n");
    printf("| ");
    padPrint("排队", 4);      printf(" | ");
    padPrint("订单号", 14);    printf(" | ");
    padPrint("姓名", 10);      printf(" | ");
    padPrint("身份证号", 20);  printf(" |\n");
    printf("+------+----------------+------------+----------------------+\n");
}

static void showStatus(void)
{
    printf("  [当前状态] 候补队列 %d 人等待 | 余票 %d 张\n", g_queue.size, g_pool);
}

/* 暂停，等待用户按回车 */
static void pauseScreen(void)
{
    char buf[8];
    printf("\n按回车键继续...");
    readLine(buf, sizeof(buf));
}

/* ========================= 四、业务功能（基础 4 项）===================== */

/* ---- 操作 1：新增候补订单（入队） ---- */
static void doAddOrder(void)
{
    printSectionTitle("新增候补订单（入队 · 队尾追加）");

    TicketOrder o;
    memset(&o, 0, sizeof(o));

    readText("  请输入旅客姓名        : ", o.name, sizeof(o.name));
    readText("  请输入 18 位身份证号  : ", o.idCard, sizeof(o.idCard));
    o.ticketCount = 1;   /* 基础版每笔订单固定 1 张 */

    generateOrderNo(o.orderNo, sizeof(o.orderNo));

    if (!queueEnqueue(&g_queue, &o)) {
        printf("\n  [错误] 内存分配失败，入队失败。\n");
        pauseScreen();
        return;
    }

    printf("\n  >>> 候补订单提交成功，已加入队尾（第 %d 位） <<<\n", g_queue.size);
    printTableHead();
    printOrderRow(g_queue.size, &o);
    printf("\n  说明：先候补先出票，系统将按排队顺序依次兑现。\n");
    showStatus();
    pauseScreen();
}

/* ---- 操作 2：余票发放（从队头依次兑现） ---- */
static void doReleaseTickets(void)
{
    printSectionTitle("余票发放（按'先到先得'从队头依次兑现）");

    showStatus();
    int k = readIntRange("\n  请输入本次释放的余票数量 k（0 表示仅查看）：", 0, 1000);

    g_pool += k;
    printf("\n  >>> 本次释放 %d 张余票，当前余票共 %d 张，开始从队头依次处理...\n\n",
           k, g_pool);

    int issued = 0;
    int hasOut = 0;

    /*
     * 核心循环（题目要求）：从队头开始，只要还有余票且队列非空，
     * 就把队头订单兑现、出队、输出信息，剩余余票减 1，继续处理下一个。
     * 余票减到 0 时循环自然结束 —— 也即「余票为 0 时判断队列是否为空」。
     */
    while (!queueEmpty(&g_queue)) {
        QNode *head = g_queue.front->next;
        if (g_pool < head->data.ticketCount) break;   /* 余票不足，停止 */

        TicketOrder cur = head->data;
        g_pool -= cur.ticketCount;

        if (!hasOut) { printTableHead(); hasOut = 1; }
        printOrderRow(++issued, &cur);

        queueDequeue(&g_queue, NULL);
    }

    if (hasOut) {
        printf("+------+----------------+------------+----------------------+\n");
    } else {
        printf("  （本次没有订单被兑现）\n");
    }

    printf("\n  ---- 本次发放结果 ----\n");
    printf("  兑现订单：%d 笔      剩余余票：%d 张\n", issued, g_pool);

    /* 操作 3 的体现：余票为 0 时判断队列是否为空 */
    if (queueEmpty(&g_queue)) {
        printf("  >> 候补队列已清空，当前无旅客在等待。\n");
        if (g_pool > 0)
            printf("  >> 余票池仍有 %d 张余票（暂无候补需求）。\n", g_pool);
    } else {
        printf("  >> 余票已用尽，候补队列仍有 %d 位旅客在等待，"
               "需等待下次放票 / 退票。\n", g_queue.size);
        printf("  >> （队列不为空，旅客继续等待；新增订单仍会从队尾入队。）\n");
    }
    showStatus();
    pauseScreen();
}

/* ============================== 五、主界面 =============================== */

static void printMenu(void)
{
    printf("\n");
    printf("==============================================================\n");
    printf("             12306 车票候补模块模拟系统  v1.0（基础版）\n");
    printf("==============================================================\n");
    printf("  【基础功能 · 题目要求】\n");
    printf("    1. 新增候补订单（入队）\n");
    printf("    2. 余票发放（从队头依次兑现）\n");
    printf("    3. 退出程序\n");
    printf("--------------------------------------------------------------\n");
    printf("  当前状态：候补 %d 人等待 | 余票 %d 张\n", g_queue.size, g_pool);
    printf("--------------------------------------------------------------\n");
}

int main(void)
{
    initConsole();

    if (!queueInit(&g_queue)) {
        printf("初始化队列失败：内存不足。\n");
        return 1;
    }

    printf("\n");
    printf("==============================================================\n");
    printf("            欢迎使用 12306 车票候补模块模拟系统\n");
    printf("   规则：旅客按提交时间先后排队，退票/放票产生的余票从队头\n");
    printf("         开始依次分配 —— 先候补先出票，绝不插队。\n");
    printf("==============================================================\n");

    char choice[16];
    int running = 1;
    while (running) {
        printMenu();
        printf("  请输入操作编号：");
        readLine(choice, sizeof(choice));

        if (strcmp(choice, "1") == 0)      doAddOrder();
        else if (strcmp(choice, "2") == 0) doReleaseTickets();
        else if (strcmp(choice, "3") == 0) {
            printf("\n  感谢使用 12306 车票候补模块模拟系统，再见！\n\n");
            running = 0;
        }
        else {
            printf("\n  [输入无效] 请输入 1 ~ 3。\n");
            pauseScreen();
        }
    }

    queueDestroy(&g_queue);
    return 0;
}
