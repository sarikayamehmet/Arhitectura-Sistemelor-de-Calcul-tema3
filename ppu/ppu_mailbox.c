#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <libspe2.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define SPU_THREADS 8
// macro for rounding input value to the next higher multiple of either 
// 16 or 128 (to fulfill MFC's DMA requirements)
#define spu_mfc_ceil128(value)  ((value + 127) & ~127)
#define spu_mfc_ceil16(value)   ((value +  15) &  ~15)

volatile char str[256]  __attribute__ ((aligned(16)));
extern spe_program_handle_t spu_mailbox;

typedef struct {
	int cellno;
	int data;
	spe_context_ptr_t spe;
} thread_arg_t;

struct pixel {
	char red;
	char green;
	char blue;
};

void *ppu_pthread_function(void *thread_arg) {
	thread_arg_t *arg = (thread_arg_t *) thread_arg;
	unsigned int entry = SPE_DEFAULT_ENTRY;
	if (spe_context_run(arg->spe, &entry, 0, (void *) arg->cellno, 
				(void *) arg->data, NULL) < 0) {
		perror ("Failed running context");
		exit (1);
	}

	pthread_exit(NULL);
}

void read_from_file(FILE *fin, struct pixel **a, long *width, long *height,
		int *max_color)
{
	char line[256];
	char *numbers, *tok;
	long line_no = 0, i = 0;

	while(fgets(line, sizeof(line), fin) != NULL) {
		/* Exclude comments */
		if (line[0] == '#')
			continue;
		/* Check if the file is ppm */
		if (!line_no && strncmp(line, "P3", 2)) {
			perror("The input file is not ppm");
			return;
		}
		line_no++;
		if (line_no == 2) {
			numbers = strtok(line, " ");
			*width = atol(numbers);
			while (1) {
				tok = strtok(NULL, " ");
				if (tok == NULL)
					break;
				*height = atol(tok);
			}
			*a = malloc(*width * *height * sizeof(struct pixel));
			if (!*a) {
				perror("Error on alocating matrix");
				return;
			}
		}
		if (line_no == 3) {
			*max_color = atoi(line);
			printf("Width = %ld, height = %ld, ", *width, *height);
			printf("Max color = %d\n", *max_color);
		}
		/* Read the RGB values */
		if (line_no > 3) {
			if (i % 3 == 0)
				(*a)[i / 3].red = atoi(line);
			if (i % 3 == 1)
				(*a)[i / 3].green = atoi(line);
			if (i % 3 == 2)
				(*a)[i / 3].blue = atoi(line);
			i++;
		}
	}
}

/* Return a random path from the pool of patches */
struct pixel *get_random_patch(struct pixel**pool, int pool_size)
{
	int pos = rand() % pool_size;
	return pool[pos];
}

struct pixel **build_pool_patches(int count, struct pixel *img, struct pixel **pool,
		int patch_w, int patch_h, int img_w, int img_h)
{
	printf("Building pool patches, h %d, w%d\n", patch_h, patch_w);
	int i, j, k;
	long rand_x, rand_y, pos_img, pos_patch;
	srand(time(NULL));
	pool = malloc(count * sizeof(struct pixel *));
	if (!pool)
		return NULL;
	for (i = 0; i < count; i++) {
		pool[i] = malloc(patch_w * patch_h * sizeof(struct pixel));
		if (!pool[i]) {
			free(pool);
			return NULL;
		}
		//printf("Successfuly allocated patch %d\n", i);
		rand_x = rand() % (img_h - patch_h);
		rand_y = rand() % (img_w - patch_w);		
		//printf("Patch starts at %ld, %ld\n", rand_x, rand_y);
		for (j = 0; j < patch_h; j++) {
			for (k = 0; k < patch_w; k++) {
				pos_img = (rand_x + j) * img_w + rand_y + k;
				pos_patch = j * patch_w + k;
				//printf("Copy pixel from img[%d] to patch[%d]\n",
				//	pos_img, pos_patch);
				pool[i][pos_patch] = img[pos_img];
			}
		}
	}	
	return pool;
}



int main(int argc, char **argv)
{
	spe_context_ptr_t ctxs[SPU_THREADS];
	pthread_t threads[SPU_THREADS];
	thread_arg_t arg[SPU_THREADS];
	spe_event_unit_t pevents[SPU_THREADS],
			 events_received[SPU_THREADS];
	spe_event_handler_ptr_t event_handler;
	event_handler = spe_event_handler_create();

	int zoom, rows, columns, overlap_vert, overlap_oriz, pos_patch;
	long height, width, i, j, k, petic_x, petic_y;
	int max_color, patch_w, patch_h, pool_size;
	struct pixel *a = NULL, *result = NULL;
	struct pixel **patches __attribute__ ((aligned(16))) = NULL;

	/* Store the arguments */
	if (argc < 8)
		perror("Insert the name of the input file");
	char *input_file = argv[1];
	char *output_file = argv[2];
	zoom = atoi(argv[3]);
	rows = atoi(argv[4]);
	columns = atoi(argv[5]);
	overlap_vert = atoi(argv[6]);
	overlap_oriz = atoi(argv[7]);
	printf("Input file name %s\n", input_file);

	/* Read the matrix */
	FILE *fin = fopen(input_file, "r");
	if (!fin)
		perror("Error while opening input file for reading");
	read_from_file(fin, &a, &width, &height, &max_color);
	printf("Access some pixel: %d, %d, %d\n", a[183].red, a[183].green,
			a[183].blue);
	printf("Successfuly read from file\n");

	/* Create a pool of several random patches */
	patch_w = width * zoom / columns;
	patch_h = height * zoom / rows;
	pool_size = rows * columns;
	patches = build_pool_patches(pool_size, a, patches, patch_w, patch_h,
			width, height);
	if (patches == NULL) {
		printf("Patches e null\n");
		return -1;
	}
	/* Print a patch from pool */
	FILE *fout = fopen("23_out.ppm", "w");
	fprintf(fout, "P3\n");
	fprintf(fout, "%d %d\n%d\n", patch_w, patch_h, max_color);
	struct pixel *patch = patches[0];
	for (i = 0; i < patch_h; i++) {
		for (j = 0; j < patch_w; j++) {
			pos_patch = i * patch_w + j;
			fprintf(fout,"%d\n%d\n%d\n", patch[pos_patch].red,
					patch[pos_patch].green, patch[pos_patch].blue);
		}
	}


	/* Create several SPE-threads to execute 'SPU'. */
	for (i = 0; i < SPU_THREADS; i++) {
		/* Create context */
		if ((ctxs[i] = spe_context_create (SPE_EVENTS_ENABLE, NULL)) == NULL) {
			perror ("Failed creating context");
			exit (1);
		}

		/* Load program into context */
		if (spe_program_load (ctxs[i], &spu_mailbox)) {
			perror ("Failed loading program");
			exit (1);
		}

		/* Send some more paramters to the SPU. */
		arg[i].cellno = i;
		arg[i].spe = ctxs[i];
		arg[i].data = 55 + i;

		/* Create thread for each SPE context */
		if (pthread_create (&threads[i], NULL, &ppu_pthread_function, &arg[i])) {
			perror ("Failed creating thread");
			exit (1);
		}
		/* The type of the event(s) we are working with */
		pevents[i].events = SPE_EVENT_OUT_INTR_MBOX;
		pevents[i].spe = ctxs[i];
		pevents[i].data.u32 = i; // just some data to pass

		spe_event_handler_register(event_handler, &pevents[i]);
	}

	/* Sending initial parameters and the first patch to each SPU */	
	struct pixel *first_patch;
	for (i = 0; i < SPU_THREADS; i++) {
		spe_in_mbox_write(ctxs[i], (void *) &rows, 1, 
			SPE_MBOX_ANY_NONBLOCKING);
		spe_in_mbox_write(ctxs[i], (void *) &patch_h, 1,
			SPE_MBOX_ANY_NONBLOCKING);
		spe_in_mbox_write(ctxs[i], (void *) &patch_w, 1,
			SPE_MBOX_ANY_NONBLOCKING);
		first_patch = get_random_patch(patches, pool_size);
		printf("PPU sends to SPU %d: %d, %d, %d\n", i, 
			first_patch[0].red, first_patch[0].green,
			first_patch[0].blue);
		int pointer = (int)first_patch;
		//printf("PPU catre SPU %d, pointer %d\n", i, pointer);
		spe_in_mbox_write(ctxs[i], (void *)&pointer, 1,
			SPE_MBOX_ANY_NONBLOCKING);	
	}


	/* Getting the offset of LS in data buffer
	unsigned int data_pointer;
	int ret=0,done=0;
	for (;done<8; ret = spe_event_wait(event_handler, events_received,
				1, 0)) {
		// do useful stuff
		if (ret == 0)
			continue;
		if (ret<0) {
			printf("Error: event wait error\n");
			continue;
		}
		if (events_received[0].events & SPE_EVENT_OUT_INTR_MBOX) {
			printf("SPU%d sent me a message\n",events_received[0].data.u32);
			// read the mailbox for the spe that issued the event
			spe_out_intr_mbox_read(events_received[0].spe, (unsigned int*)&data_pointer, 1, SPE_MBOX_ANY_BLOCKING);
			printf("SPU%d says: %d\n", events_received[0].data.u32, data_pointer);
			done += 1;
		}
	}*/

	/* Wait for SPU-thread to complete execution. */
	for (i = 0; i < SPU_THREADS; i++) {
		if (pthread_join(threads[i], NULL)) {
			perror("Failed pthread_join");
			exit(1);
		}

		/* Destroy context */
		if (spe_context_destroy(ctxs[i]) != 0) {
			perror("Failed destroying context");
			exit(1);
		}
	}

	printf("\nThe program has successfully executed.\n");
	return 0;
}

