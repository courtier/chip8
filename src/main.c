#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

#include <SDL2/SDL.h>

/*compare: https://colineberhardt.github.io/wasm-rust-chip8/web/*/

/*64x32 is too small, so we multiply by this amount*/
#define SCREEN_MULTIPLIER 10
/*64 pixels original, we do 64*MULTIPLIER*/
#define SCREEN_WIDTH 64 * SCREEN_MULTIPLIER
/*32 pixels original, we do 32*MULTIPLIER*/
#define SCREEN_HEIGHT 32 * SCREEN_MULTIPLIER
/*60hz frequency in nanoseconds*/
/*change to this -> on Chip8 they count down in threes using the PC's 18.2Hz Clock (http://devernay.free.fr/hacks/chip8/chip8def.htm)*/
#define UPDATE_FREQUENCY 1 / 60 * 10000000

/*memory*/
#define INTERPRETER_START 0x000
#define INTERPRETER_END 0x1FF
#define GAME_START 0x200
#define GAME_END 0xFFF

/*font*/
#define FONT_SIZE 80

/*keypad*/
const static char KEYMAP[16] = {
    SDLK_x, // 0
    SDLK_1, // 1
    SDLK_2, // 2
    SDLK_3, // 3
    SDLK_q, // 4
    SDLK_w, // 5
    SDLK_e, // 6
    SDLK_a, // 7
    SDLK_s, // 8
    SDLK_d, // 9
#ifdef GERMAN
    SDLK_y, // A
#else
    SDLK_z, // A
#endif
    SDLK_c, // B
    SDLK_4, // C
    SDLK_r, // D
    SDLK_f, // E
    SDLK_v  // F
};

/*fontset*/
const static uint8_t FONTSET[80] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, // 0
    0x20, 0x60, 0x20, 0x20, 0x70, // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, // 3
    0x90, 0x90, 0xF0, 0x10, 0x10, // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, // 6
    0xF0, 0x10, 0x20, 0x40, 0x40, // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90, // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, // B
    0xF0, 0x80, 0x80, 0x80, 0xF0, // C
    0xE0, 0x90, 0x90, 0x90, 0xE0, // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, // E
    0xF0, 0x80, 0xF0, 0x80, 0x80  // F
};

typedef struct
{
    unsigned int I;
    unsigned int PC;
    unsigned int SP;
    unsigned long DT;
    unsigned long ST;
    unsigned int Stack[16];
    unsigned char VN[16];
    unsigned char memory[0xFFF];
    bool screen[64][32];
    bool keypad[16];
    bool draw_screen;
} Machine;

void multiply_pixels(SDL_Rect *rect, int x, int y);
int load_rom(Machine *machine, char *path);
int run_instruction(Machine *machine);
void draw_instruction(Machine *machine, unsigned int x, unsigned int y, unsigned int n);

void handler(int sig)
{
    void *array[10];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 10);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

int main(int argc, char *argv[])
{
    signal(SIGSEGV, handler); // install our handler
    char window_name[255];
    unsigned long cile_frequency = 1 / 700 * 10000000;
    SDL_Window *window;
    SDL_Renderer *renderer;

    struct timespec last_frame_time;
    struct timespec current_frame_time;

    Machine machine = {.PC = GAME_START, .I = 0, .draw_screen = false, .SP = 0, .DT = 0, .ST = 0};
    memset(machine.screen, false, sizeof(machine.screen));
    memset(machine.memory, 0, sizeof(machine.memory));
    memset(machine.Stack, 0, sizeof(machine.Stack));
    memset(machine.VN, 0, sizeof(machine.VN));
    memset(machine.keypad, false, sizeof(machine.keypad));

    if (argc < 2)
    {
        fprintf(stderr, "usage: ./chip8 <path to rom> [opt: cile freq. - DEF: 700hz]\n");
        return 1;
    }
    if (sizeof(argv[1]) + 17 > 255)
    {
        fprintf(stderr, "error rom name too long\n");
        return 1;
    }
    if (load_rom(&machine, argv[1]) < 0)
    {
        fprintf(stderr, "error load_rom\n");
        return 1;
    }
    if (sprintf(window_name, "CHIP8 Emulator - %s", argv[1]) < 0)
    {
        fprintf(stderr, "error sprintf\n");
        return 1;
    }
    if (SDL_Init(SDL_INIT_EVERYTHING) < 0)
    {
        fprintf(stderr, "error sdl_init\n");
        return 1;
    }
    window = SDL_CreateWindow(window_name, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
    if (!window)
    {
        fprintf(stderr, "error SDL_CreateWindow\n");
        return 1;
    }
    renderer = SDL_CreateRenderer(window, 0, SDL_RENDERER_ACCELERATED);
    if (!renderer)
    {
        fprintf(stderr, "error SDL_CreateRenderer\n");
        return 1;
    }

    for (int i = 0; i < FONT_SIZE; i++)
    {
        machine.memory[i] = FONTSET[i];
    }

    SDL_Rect rectangles[64][32];
    for (int x = 0; x < 64; x++)
    {
        for (int y = 0; y < 32; y++)
        {
            multiply_pixels(&rectangles[x][y], x, y);
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &last_frame_time) < 0)
    {
        fprintf(stderr, "error clock_gettime\n");
        return 1;
    }
    for (;;)
    {
        SDL_Event sdl_event;
        while (SDL_PollEvent(&sdl_event) != 0)
        {
            switch (sdl_event.type)
            {
            case SDL_QUIT:
                goto end;
                break;
            case SDL_KEYDOWN:
                for (int i = 0; i < 16; i++)
                {
                    if (sdl_event.key.keysym.sym == KEYMAP[i])
                    {
                        machine.keypad[i] = true;
                    }
                }
                break;
            case SDL_KEYUP:
                for (int i = 0; i < 16; i++)
                {
                    if (sdl_event.key.keysym.sym == KEYMAP[i])
                    {
                        machine.keypad[i] = false;
                    }
                }
                break;
            }
        }

        clock_gettime(CLOCK_MONOTONIC_RAW, &current_frame_time);
        if (current_frame_time.tv_nsec - last_frame_time.tv_nsec >= cile_frequency)
        {
            /*process instruction*/
            run_instruction(&machine);
        }
        if (current_frame_time.tv_nsec - last_frame_time.tv_nsec >= UPDATE_FREQUENCY)
        {
            if (machine.DT > 0)
            {
                machine.DT--;
            }
            if (machine.ST > 0)
            {
                machine.ST--;
            }
            if (machine.draw_screen)
            {
                for (int x = 0; x < 64; x++)
                {
                    for (int y = 0; y < 32; y++)
                    {
                        if (machine.screen[x][y] == true)
                        {
                            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                            /*printf("x: %d, y: %d, rx: %d, ry: %d\n", x, y, rectangles[x][y].x, rectangles[x][y].y);*/
                        }
                        else
                        {
                            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                        }
                        SDL_RenderFillRect(renderer, &rectangles[x][y]);
                    }
                }
                SDL_RenderPresent(renderer);
                machine.draw_screen = false;
            }
        }
        last_frame_time = current_frame_time;
    }

end:
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

int run_instruction(Machine *machine)
{
    short opcode;
    unsigned int nnn, x, kk, y, n;
    int sum;
    SDL_Event sdl_event;
    if (machine->PC + 1 > GAME_END)
    {
        return -1;
    }
    //opcode = machine->memory[machine->PC] & machine->memory[machine->PC + 1];
    opcode = machine->memory[machine->PC] << 8 | machine->memory[machine->PC + 1];
    srand(time(NULL));
    switch (opcode & 0xF000)
    {
    case 0x0000:
        printf("0\n");
        switch (opcode & 0x00FF)
        {
        case 0x00E0:
            memset(machine->screen, false, sizeof(machine->screen));
            break;
        case 0x00EE:
            machine->PC = machine->Stack[machine->SP & 0xF];
            machine->SP--;
            break;
        default:
            printf("unknown opcode, skippinddddg\n");
            break;
        }
        break;
    case 0x1000:
        printf("1\n");
        nnn = opcode & 0x0FFF;
        machine->PC = nnn;
        return 0;
        break;
    case 0x2000:
        printf("2\n");
        nnn = opcode & 0x0FFF;
        machine->SP++;
        machine->Stack[machine->SP & 0xF] = machine->PC;
        machine->PC = nnn;
        return 0;
        break;
    case 0x3000:
        printf("3\n");
        x = (opcode & 0x0F00) >> 8;
        kk = opcode & 0x00FF;
        if (machine->VN[x] == kk)
        {
            machine->PC += 2;
        }
        return 0;
        break;
    case 0x4000:
        printf("4\n");
        x = (opcode & 0x0F00) >> 8;
        kk = opcode & 0x00FF;
        if (machine->VN[x] != kk)
        {
            machine->PC += 2;
        }
        break;
    case 0x5000:
        printf("5\n");
        x = (opcode & 0x0F00) >> 8;
        y = (opcode & 0x00F0) >> 4;
        if (machine->VN[x] == machine->VN[y])
        {
            machine->PC += 2;
        }
        break;
    case 0x6000:
        printf("6\n");
        x = (opcode & 0x0F00) >> 8;
        kk = opcode & 0x00FF;
        machine->VN[x] = kk;
        break;
    case 0x7000:
        printf("7\n");
        x = (opcode & 0x0F00) >> 8;
        kk = opcode & 0x00FF;
        machine->VN[x] += kk;
        break;
    case 0x8000:
        printf("8\n");
        switch (opcode & 0x000F)
        {
        case 0x000:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            machine->VN[x] = machine->VN[y];
            break;
        case 0x001:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            machine->VN[x] = machine->VN[x] | machine->VN[y];
            break;
        case 0x002:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            machine->VN[x] = machine->VN[x] & machine->VN[y];
            break;
        case 0x003:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            machine->VN[x] = machine->VN[x] ^ machine->VN[y];
            break;
        case 0x004:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            sum = machine->VN[x] + machine->VN[y];
            if (sum > 255)
            {
                machine->VN[0xF] = 1;
            }
            else
            {
                machine->VN[0xF] = 0;
            }
            machine->VN[x] = sum & 0xFF;
            break;
        case 0x005:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            if (machine->VN[x] > machine->VN[y])
            {
                machine->VN[0xF] = 1;
            }
            else
            {
                machine->VN[0xF] = 0;
            }
            machine->VN[x] -= machine->VN[y];
            break;
        /*TODO** https://www.reddit.com/r/EmuDev/comments/8cbvz6/chip8_8xy6/ https://www.reddit.com/r/EmuDev/comments/72dunw/chip8_8xy6_help/*/
        case 0x006:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            if (machine->VN[x] % 2 == 1)
            {
                machine->VN[0xF] = 1;
            }
            else
            {
                machine->VN[0xF] = 0;
            }
            machine->VN[x] *= 0.5;
            break;
        case 0x007:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            if (machine->VN[y] > machine->VN[x])
            {
                machine->VN[0xF] = 1;
            }
            else
            {
                machine->VN[0xF] = 0;
            }
            machine->VN[x] = machine->VN[y] - machine->VN[x];
            break;
        case 0x00E:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;
            if (machine->VN[x] % 2 == 1)
            {
                machine->VN[0xF] = 1;
            }
            else
            {
                machine->VN[0xF] = 0;
            }
            machine->VN[x] *= 2;
            break;
        default:
            printf("unknown opcode, skipping\n");
            break;
        }
        break;
    case 0x9000:
        printf("9\n");
        x = (opcode & 0x0F00) >> 8;
        y = (opcode & 0x00F0) >> 4;
        if (machine->VN[x] != machine->VN[y])
        {
            machine->PC += 2;
        }
        break;
    case 0xA000:
        printf("a\n");
        nnn = opcode & 0x0FFF;
        machine->I = nnn;
        break;
    case 0xB000:
        printf("b\n");
        nnn = opcode & 0x0FFF;
        machine->PC = nnn + machine->VN[0x0];
        break;
    case 0xC000:
        printf("c\n");
        x = (opcode & 0x0F00) >> 8;
        kk = opcode & 0x00FF;
        machine->VN[x] = (rand() % 254) & kk;
        break;
    case 0xD000:
        printf("d\n");
        x = (opcode & 0x0F00) >> 8;
        y = (opcode & 0x00F0) >> 4;
        n = opcode & 0x000F;
        draw_instruction(machine, x, y, n);
        machine->draw_screen = true;
        break;
    case 0xE000:
        printf("e\n");
        switch (opcode & 0x00FF)
        {
        case 0x009E:
            x = (opcode & 0x0F00) >> 8;
            if (machine->keypad[machine->VN[x]] == true)
            {
                machine->PC += 2;
            }
            break;
        case 0x00A1:
            x = (opcode & 0x0F00) >> 8;
            if (machine->keypad[machine->VN[x]] != true)
            {
                machine->PC += 2;
            }
            break;
        default:
            printf("unknown opcode, skipping\n");
            break;
        }
        break;
    case 0xF000:
        printf("f\n");
        switch (opcode & 0x00FF)
        {
        case 0x0007:
            x = (opcode & 0x0F00) >> 8;
            machine->VN[x] = machine->DT;
            break;
        case 0x000A:
            x = (opcode & 0x0F00) >> 8;
            for (;;)
            {
                while (SDL_PollEvent(&sdl_event) != 0)
                {
                    switch (sdl_event.type)
                    {
                    case SDL_KEYDOWN:
                        for (int i = 0; i < 16; i++)
                        {
                            if (sdl_event.key.keysym.sym == KEYMAP[i])
                            {
                                machine->VN[x] = i;
                                break;
                            }
                        }
                        goto skip_event_loop;
                        break;
                    }
                }
            }
        skip_event_loop:
            break;
        case 0x0015:
            x = (opcode & 0x0F00) >> 8;
            machine->DT = machine->VN[x];
            break;
        case 0x0018:
            x = (opcode & 0x0F00) >> 8;
            machine->ST = machine->VN[x];
            break;
        case 0x001E:
            x = (opcode & 0x0F00) >> 8;
            machine->I += machine->VN[x];
            break;
        case 0x0029:
            x = (opcode & 0x0F00) >> 8;
            machine->I += machine->VN[x];
            break;
        case 0x0033:
            x = (opcode & 0x0F00) >> 8;
            machine->memory[machine->I] = machine->VN[x] / 100;
            machine->memory[machine->I + 1] = machine->VN[x] % 10;
            machine->memory[machine->I + 2] = machine->VN[x] % 10;
            break;
        case 0x0055:
            x = (opcode & 0x0F00) >> 8;
            for (int i = 0; i <= x; i++)
            {
                machine->memory[machine->I + i] = machine->VN[i];
            }
            break;
        case 0x0065:
            x = (opcode & 0x0F00) >> 8;
            for (int i = 0; i <= x; i++)
            {
                machine->VN[i] = machine->memory[machine->I + i];
            }
            break;
        default:
            printf("unknown opcode, skipping\n");
            break;
        }
        break;

    default:
        printf("unknown opcode, skippsdsding\n");
        break;
    }
    printf("%d\n", machine->PC);
    machine->PC += 2;
    return 0;
}

void draw_instruction(Machine *machine, unsigned int x, unsigned int y, unsigned int n)
{
    unsigned int i, j;
    unsigned int xc, yc;
    unsigned char pixel;

    xc = machine->VN[x];
    yc = machine->VN[y];

    machine->VN[0xF] = false;
    for (i = 0; i < n; i++)
    {
        pixel = machine->memory[machine->I + i];
        for (j = 0; j < 8; j++)
        {
            if (pixel & (0x80 >> j))
            {
                if (machine->screen[j + xc][i + yc])
                {
                    machine->VN[0xF] = true;
                }
                machine->screen[j + xc][i + yc] ^= 1;
            }
        }
    }
}

void multiply_pixels(SDL_Rect *rect, int x, int y)
{
    rect->x = x * SCREEN_MULTIPLIER;
    rect->y = y * SCREEN_MULTIPLIER;
    rect->w = SCREEN_MULTIPLIER;
    rect->h = SCREEN_MULTIPLIER;
}

int load_rom(Machine *machine, char *path)
{
    FILE *rom_file;
    unsigned long rom_length;
    char *rom_buffer;
    rom_file = fopen(path, "rb\n");
    if (!rom_file)
    {
        return -1;
    }
    if (fseek(rom_file, 0, SEEK_END))
    {
        return -1;
    }
    rom_length = ftell(rom_file);
    if (GAME_END - GAME_START < rom_length)
    {
        return -1;
    }

    rewind(rom_file);
    rom_buffer = (char *)malloc(sizeof(char) * rom_length);
    if (!rom_buffer)
    {
        return -1;
    }
    fread(rom_buffer, sizeof(int), rom_length, rom_file);
    int i = 0;
    while (i < rom_length)
    {
        machine->memory[GAME_START + i] = rom_buffer[i];
        i++;
    }
    fclose(rom_file);
    return 0;
}