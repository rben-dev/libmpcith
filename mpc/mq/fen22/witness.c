#include "witness.h"
#include "field.h"
#include <stdio.h>

void uncompress_instance(instance_t* inst) {
    if(inst->A == NULL) {
        // We assume here that
        //   inst->A == NULL iff inst->b == NULL
        inst->A = (void*) malloc(PARAM_m*PARAM_n*PARAM_n);
        inst->b = (void*) malloc(PARAM_m*PARAM_n);
        prg_context entropy_ctx;
        samplable_t entropy = prg_to_samplable(&entropy_ctx);
        prg_init(&entropy_ctx, inst->seed, NULL);
        vec_rnd((point_t*) (*inst->A), PARAM_m*PARAM_n*PARAM_n, &entropy);
        vec_rnd((point_t*) (*inst->b), PARAM_m*PARAM_n, &entropy);
    }
}

void generate_instance_with_solution(instance_t** inst, solution_t** sol, samplable_t* entropy) {
    // Allocate
    *sol = (solution_t*) malloc(sizeof(solution_t));
    *inst = (instance_t*) malloc(sizeof(instance_t));
    (*inst)->A = NULL;
    (*inst)->b = NULL;

    // Extended Witness
    vec_rnd((*sol)->x, PARAM_n, entropy);

    // Sample a seed
    byte_sample(entropy, (*inst)->seed, PARAM_SEED_SIZE);

    // Build random matrices
    uncompress_instance(*inst);

    // Build y
    point_t v[PARAM_n];
    for(int i=0; i<PARAM_m; i++) {
        memcpy(v, (*(*inst)->b)[i], PARAM_n);
        matcols_muladd(v, (*sol)->x, (point_t*) (*(*inst)->A)[i], PARAM_n, PARAM_n);
        (*inst)->y[i] = 0;
        matcols_muladd(&(*inst)->y[i], (*sol)->x, v, PARAM_n, 1);
    }
}

void hash_update_instance(hash_context* ctx, const instance_t* inst) {
    hash_update(ctx, inst->seed, PARAM_SEED_SIZE);
    hash_update(ctx, inst->y, PARAM_m);
}

void serialize_instance(uint8_t* buf, const instance_t* inst) {
    memcpy(buf, inst->seed, PARAM_SEED_SIZE);
    memcpy(buf + PARAM_SEED_SIZE, inst->y, PARAM_m);
}

instance_t* deserialize_instance(const uint8_t* buf) {
    instance_t* inst = (instance_t*) malloc(sizeof(instance_t));
    inst->A = NULL;
    inst->b = NULL;
    memcpy(inst->seed, buf, PARAM_SEED_SIZE);
    memcpy(inst->y, buf + PARAM_SEED_SIZE, PARAM_m);
    return inst;
}

void serialize_instance_solution(uint8_t* buf, const solution_t* sol) {
    memcpy(buf, sol->x, PARAM_n);
}

solution_t* deserialize_instance_solution(const uint8_t* buf) {
    solution_t* sol = (solution_t*) malloc(sizeof(solution_t));
    memcpy(sol->x, buf, PARAM_n);
    return sol;
}

void free_instance_solution(solution_t* sol) {
    free(sol);
}

void free_instance(instance_t* inst) {
    if(inst->A) {
        free(inst->A);
        inst->A = NULL;
    }
    if(inst->b) {
        free(inst->b);
        inst->b = NULL;
    }
    free(inst);
}

int are_same_instances(instance_t* inst1, instance_t* inst2) {
    int ret = 0;
    ret |= memcmp(inst1->seed, inst2->seed, PARAM_SEED_SIZE);
    ret |= memcmp(inst1->y, inst2->y, PARAM_m);
    return ((ret) ? 0 : 1);
}

int is_correct_solution(instance_t* inst, solution_t* sol) {
    int ret = 0;
    uncompress_instance(inst);

    point_t y_candidate[PARAM_m];
    point_t v[PARAM_n];
    for(int i=0; i<PARAM_m; i++) {
        memcpy(v, (*inst->b)[i], PARAM_n);
        matcols_muladd(v, sol->x, (point_t*) (*inst->A)[i], PARAM_n, PARAM_n);
        y_candidate[i] = 0;
        matcols_muladd(&y_candidate[i], sol->x, v, PARAM_n, 1);
    }
    ret = memcmp(inst->y, y_candidate, PARAM_m);

    return (ret == 0);
}
