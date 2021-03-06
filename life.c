/*****************************************************************************
 * life.c
 * Parallelized and optimized implementation of the game of life resides here
 ****************************************************************************/
#include "life.h"
#include "util.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>


/*****************************************************************************
 * Helper function definitions
 ****************************************************************************/

//This data structure contains all arguments needed
//by the worker function.
struct thread_argument_t {
    char *outboard;
    char *inboard;
    int nrows;
    int ncols;
    int ith_slice;
    int gens_max;
    pthread_barrier_t *barrier;
    pthread_mutex_t *boundary_locks;
};


// Function to apply our encoding format to the board
// Behavior: does not alter the content of the inboard. Only the outboard is
// updated with encoding ==> so at the end we copy the outboard to the inboard
void preprocessing_board(char *inboard, char *outboard, int size) {
    char *board = inboard;
    int i, j, total_size = size * size;
    for (i = 0; i < total_size; i++) {
        if (board[i] == 1) {//if cell is alive
            board[i] = 0;//clear the cell first
            SPAWN(board[i]);//mark the cell as live in our format
        }
    }

    int inorth, isouth, jwest, jeast;
    for (i = 0; i < size; i++) {
        for (j = 0; j < size; j++) {
            if (ALIVE(BOARD(board, i, j))) {

		//Updates its neighbours with increased live cell count
                jwest = mod(j - 1, size);
                jeast = mod(j + 1, size);
                inorth = mod(i - 1, size);
                isouth = mod(i + 1, size);

                N_INC(board, inorth, jwest);
                N_INC(board, inorth, j);
                N_INC(board, inorth, jeast);
                N_INC(board, i, jwest);
                N_INC(board, i, jeast);
                N_INC(board, isouth, jwest);
                N_INC(board, isouth, j);
                N_INC(board, isouth, jeast);
            }
        }
    }

    //copy inboard to outboard to make both have the same content
    memmove(outboard, inboard, size * size * sizeof(char));
}

/**
 * Function to revert the encoding that we applied in preprocessing stage.
 * */
inline void postprocessing_board(char *board, int nrows, int ncols) {
    int total_size = nrows * ncols;
    for (int i = 0; i < total_size; ++i) {
        board[i] = ALIVE(board[i]);
    }
}

/**
 * Worker function to be invoked by threads
 * */
void *worker_fuction_by_rows_encoding(void *args) {
    struct thread_argument_t *arg = (struct thread_argument_t *) args;
    char *outboard = arg->outboard;
    char *inboard = arg->inboard;
    int nrows = arg->nrows;
    int ncols = arg->ncols;
    int ith_slice = arg->ith_slice;
    int gens_max = arg->gens_max;
    pthread_barrier_t *barrier = arg->barrier;
    pthread_mutex_t *boundary_locks = arg->boundary_locks;
    pthread_mutex_t *boundary_locks_upper = &boundary_locks[ith_slice];
    pthread_mutex_t *boundary_locks_lower = &boundary_locks[(ith_slice + 1) % NUM_THREADS];
    int slice_size = arg->nrows / NUM_THREADS;
    int start = ith_slice * slice_size;
    int end = start + slice_size;

    for (int curgen = 0; curgen < gens_max; ++curgen) {
        int i, j;
        for (j = 0; j < ncols; j++) {

	    /*The two columns near the boundary of the slices assigned
	      to different worker threads needs to be locked
	      in order to avoid race conditions.
	    */	    
            for (i = start; i < start + 2; i++) {
		//lock upper boundary
		if(ALIVE(BOARD(inboard, i, j))) {
		    if(TOKILL(BOARD(inboard, i, j))) {
			pthread_mutex_lock(boundary_locks_upper);
			kill_cell(outboard, i, j, nrows);
			pthread_mutex_unlock(boundary_locks_upper);
		    }
		}
		else {
		    if(TOSPAWN(BOARD(inboard, i, j))) {
			pthread_mutex_lock(boundary_locks_upper);
			spawn_cell(outboard, i, j, nrows);
			pthread_mutex_unlock(boundary_locks_upper);
			
		    }
		}
		
            }

	    /*As for the main body, since any changes are made on the
	      outboard, the inboard remains read-only and thus there
	      are no worries about race conditions.
	     */
            for (i = start + 2; i < end - 2; i++) {
                do_cell(outboard, inboard, i, j, nrows);
		
            }
	    
            for (i = end - 2; i < end; i++) {
                // lock lower boundary
		if(ALIVE(BOARD(inboard, i, j))) {
		    if(TOKILL(BOARD(inboard, i, j))) {
			pthread_mutex_lock(boundary_locks_lower);
			kill_cell(outboard, i, j, nrows);
			pthread_mutex_unlock(boundary_locks_lower);
		    }
		}
		else {
		    if(TOSPAWN(BOARD(inboard, i, j))) {
			pthread_mutex_lock(boundary_locks_lower);
			spawn_cell(outboard, i, j, nrows);
			pthread_mutex_unlock(boundary_locks_lower);
			
		    }
		}
		
            }
        }
        pthread_barrier_wait(barrier);
        // always make sure that at the start of every generation we have inboard and outboard to be
        // exactly the same
        memcpy(inboard + start * ncols, outboard + start * ncols, slice_size * ncols * sizeof(char));

        pthread_barrier_wait(barrier);
    }
    return NULL;
}


/*****************************************************************************
 * Game of life implementation
 ****************************************************************************/
char *
game_of_life(char *outboard,
             char *inboard,
             const int nrows,
             const int ncols,
             const int gens_max) {
    if (nrows < 32) {
        return sequential_game_of_life(outboard, inboard, nrows, ncols, gens_max);
    }
    else if ((nrows > 10000) || (ncols > 10000)) {
	//if the size exceeds the limit, a null pointer is returned to gol
	//which will handle the null condition
        return NULL;
    }
    else {
        return parallel_game_of_life(outboard, inboard, nrows, ncols, gens_max);
    }
}

/**
 * The main part of parallel version of GOL
 * */
char *parallel_game_of_life(char *outboard,
                            char *inboard,
                            const int nrows,
                            const int ncols,
                            const int gens_max) {
    // Synchronization primitives
    pthread_t worker_threads[NUM_THREADS];

    pthread_mutex_t boundary_locks[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_mutex_init(&boundary_locks[i], NULL);
    }
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, NUM_THREADS);

    LDA = nrows;
    // apply encoding to the board
    preprocessing_board(inboard, outboard, nrows);

    struct thread_argument_t args[NUM_THREADS];
    // create and start threads
    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].outboard = outboard;
        args[i].inboard = inboard;
        args[i].nrows = nrows;
        args[i].ncols = ncols;
        args[i].ith_slice = i;
        args[i].gens_max = gens_max;
        args[i].barrier = &barrier;
        args[i].boundary_locks = boundary_locks;
        pthread_create(&worker_threads[i], NULL, worker_fuction_by_rows_encoding, &args[i]);
    }
    //join threads before return
    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(worker_threads[i], NULL);
    }

    // reverse the board to original encoding
    postprocessing_board(outboard, LDA, LDA);

    return outboard;
}


void do_cell(char *outboard, char *inboard, int i, int j, const int size) {
    int inorth, isouth, jwest, jeast;
    char cell = BOARD(inboard, i, j);
    if (ALIVE(cell)) {
        if (TOKILL(cell)) {
            // if this cell is alive and it should die, then we need to
            // do two things: mark it alive....
            KILL(BOARD(outboard, i, j));

            // ... and decrement the counter in its neighbors
            jwest = mod(j - 1, size);
            jeast = mod(j + 1, size);
            inorth = mod(i - 1, size);
            isouth = mod(i + 1, size);

            N_DEC(outboard, inorth, jwest);
            N_DEC(outboard, inorth, j);
            N_DEC(outboard, inorth, jeast);
            N_DEC(outboard, i, jwest);
            N_DEC(outboard, i, jeast);
            N_DEC(outboard, isouth, jwest);
            N_DEC(outboard, isouth, j);
            N_DEC(outboard, isouth, jeast);

        }
    }
    else {
        // this cell is dead
        if (TOSPAWN(cell)) {

            SPAWN(BOARD(outboard, i, j));

            jwest = mod(j - 1, size);
            jeast = mod(j + 1, size);
            inorth = mod(i - 1, size);
            isouth = mod(i + 1, size);

            N_INC(outboard, inorth, jwest);
            N_INC(outboard, inorth, j);
            N_INC(outboard, inorth, jeast);
            N_INC(outboard, i, jwest);
            N_INC(outboard, i, jeast);
            N_INC(outboard, isouth, jwest);
            N_INC(outboard, isouth, j);
            N_INC(outboard, isouth, jeast);
        }
    }
}

void kill_cell(char *outboard, int i, int j, const int size) {
    int inorth, isouth, jwest, jeast;
    //char out_cell =  BOARD(outboard, i, j);
    
    KILL(BOARD(outboard, i, j));
    
    // ... and decrement the counter in its neighbors
    jwest = mod(j - 1, size);
    jeast = mod(j + 1, size);
    inorth = mod(i - 1, size);
    isouth = mod(i + 1, size);
    
    N_DEC(outboard, inorth, jwest);
    N_DEC(outboard, inorth, j);
    N_DEC(outboard, inorth, jeast);
    N_DEC(outboard, i, jwest);
    N_DEC(outboard, i, jeast);
    N_DEC(outboard, isouth, jwest);
    N_DEC(outboard, isouth, j);
    N_DEC(outboard, isouth, jeast);
    
}

void spawn_cell(char *outboard, int i, int j, const int size) {
    int inorth, isouth, jwest, jeast;
    //char out_cell = BOARD(outboard, i, j);
    
    SPAWN(BOARD(outboard, i, j));
    
    jwest = mod(j - 1, size);
    jeast = mod(j + 1, size);
    inorth = mod(i - 1, size);
    isouth = mod(i + 1, size);
    
    N_INC(outboard, inorth, jwest);
    N_INC(outboard, inorth, j);
    N_INC(outboard, inorth, jeast);
    N_INC(outboard, i, jwest);
    N_INC(outboard, i, jeast);
    N_INC(outboard, isouth, jwest);
    N_INC(outboard, isouth, j);
    N_INC(outboard, isouth, jeast);
    
}
