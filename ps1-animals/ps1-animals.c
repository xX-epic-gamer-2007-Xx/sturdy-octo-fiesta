/* If you want to actually attack this program, we recommend compiling
   it with the flag -no-pie so that the program code is not
   position-independent and does not move under ASLR. */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

typedef void (*toes_func)(void);

void even_toes(void) {
    printf("with even-toed hoofs");
}

void odd_toes(void) {
    printf("with odd-toed hoofs");
}

/* For the attack you describe in your submission, assume this
   function has the address 0x4012ce. For testing an attack, you can
   use whatever address it has in your compiled binary. */
void shellcode(void) {
    printf("Uh-oh, this looks like some sort of attack\n");
    exit(42);
}

struct herbivore {
    struct herbivore *next;
    toes_func func;
};

struct herbivore *herbivore_list = 0;

struct carnivore {
    struct carnivore *next;
    long num_teeth;
};

struct carnivore *carnivore_list = 0;

#define NUM_ANIMALS 256

/* Initialized to all null pointers */
void *animals_by_num[NUM_ANIMALS];

void new_herbivore(long x) {
    int loc = x & (NUM_ANIMALS - 1);
    struct herbivore *hp = malloc(sizeof(struct herbivore));
    printf("Allocating herbivore at %p\n", hp);
    if (!hp)
        exit(1);
    if (x & 1)
        hp->func = odd_toes;
    else
        hp->func = even_toes;
    hp->next = herbivore_list;
    herbivore_list = hp;
    animals_by_num[loc] = hp;
}

void new_carnivore(long x) {
    int loc = x & (NUM_ANIMALS - 1);
    struct carnivore *cp = malloc(sizeof(struct carnivore));
    printf("Allocating carnivore at %p\n", cp);
    if (!cp)
        exit(1);
    cp->num_teeth = x;
    cp->next = carnivore_list;
    carnivore_list = cp;
    animals_by_num[loc] = cp;
}

void release_animal(long x) {
    int loc = x & (NUM_ANIMALS - 1);
    printf("Releasing animal #%d at %p\n", loc, animals_by_num[loc]);
    if (!animals_by_num[loc]) {
        fprintf(stderr, "Attempt to release non-existant animal\n");
        exit(1);
    }
    free(animals_by_num[loc]);
    animals_by_num[loc] = 0;
}

void list_animals(void) {
    struct herbivore *hp;
    struct carnivore *cp;
    for (hp = herbivore_list; hp; hp = hp->next) {
        printf("A herbivore ");
        (hp->func)();
        printf("\n");
    }

    for (cp = carnivore_list; cp; cp = cp->next) {
        printf("A carnivore with %ld teeth\n", cp->num_teeth);
    }
}

void syntax_error(void) {
    fprintf(stderr, "Unrecognized syntax\n");
    exit(1);
}

int main(int argc, char **argv) {
    for (;;) {
        int c = getchar();
        long x;
        while (isspace(c))
            c = getchar();
        if (c == EOF)
            return 0;
        switch (c) {
        case 'h':
            if (scanf(" %li", &x) != 1)
                syntax_error();
            new_herbivore(x);
            break;
        case 'c':
            if (scanf(" %li", &x) != 1)
                syntax_error();
            new_carnivore(x);
            break;
        case 'r':
            if (scanf(" %li", &x) != 1)
                syntax_error();
            release_animal(x);
            break;
        case 'l':
            list_animals();
            break;
        case 'q':
            return 0;
            break;
        default:
            fprintf(stderr, "Unrecognized command %c\n", c);
            exit(1);
        }
    }
}
