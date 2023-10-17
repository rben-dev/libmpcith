#include "sign-mpcith.h"
#include "sample.h"
#include "tree.h"
#include "commit.h"
#include "hash.h"
#include "keygen.h"
#include "mpc.h"
#include "views.h"
#include "benchmark.h"
#include "parameters-all.h"
#include "serialization.h"
#include "sign-mpcith-additive-based.h"
#include "packing-struct.h"
#include <string.h>
#include <stdio.h>

/***********************************************
 *            Signature Structure              *
 ***********************************************/

typedef struct proof_additive_based_7r_t {
    uint8_t* seed_info; // Information required to compute the tree with seeds of of all opened parties
    uint32_t len_seed_info; // Length of seed_info buffer
    uint8_t* unopened_digest;
    uint8_t* hint_digest;

    // Plaintext broadcast messages
    vec_broadcast_t* plain_broadcast;
    
    // Last party's share
    vec_wit_t* wit;
    vec_hint_t* hint;
} proof_additive_based_7r_t;

typedef struct signature_additive_based_7r_t {
    uint8_t* salt;
    uint8_t* mpc_challenge_1_hash;
    uint8_t* mpc_challenge_2_hash;
    uint8_t* view_challenge_hash;
    proof_additive_based_7r_t proofs[PARAM_NB_EXECUTIONS];
    uint8_t* allocated_memory; // Just to manage the memory
} signature_additive_based_7r_t;

// For parsing
typedef struct const_proof_additive_based_7r_t {
    const uint8_t* seed_info; // Information required to compute the tree with seeds of of all opened parties
    uint32_t len_seed_info; // Length of seed_info buffer
    const uint8_t* unopened_digest;
    const uint8_t* hint_digest;

    // Plaintext broadcast messages
    vec_broadcast_t* plain_broadcast;
    
    // Last party's share
    const vec_wit_t* wit;
    const vec_hint_t* hint;
} const_proof_additive_based_7r_t;

typedef struct const_signature_additive_based_7r_t {
    const uint8_t* salt;
    const uint8_t* mpc_challenge_1_hash;
    const uint8_t* mpc_challenge_2_hash;
    const uint8_t* view_challenge_hash;
    const_proof_additive_based_7r_t proofs[PARAM_NB_EXECUTIONS];
    uint8_t* allocated_memory; // Just to manage the memory
} const_signature_additive_based_7r_t;

// Free signature structure
void free_signature(signature_additive_based_7r_t* sig);
void free_const_signature(const_signature_additive_based_7r_t* sig);

// For signing
signature_additive_based_7r_t* init_signature_structure(const uint8_t* salt, uint8_t* buf, size_t buflen);
int build_signature(const signature_additive_based_7r_t* sig, uint8_t* buf, size_t buflen, uint16_t hidden_views[PARAM_NB_EXECUTIONS]);

// For verification
const_signature_additive_based_7r_t* parse_signature(const uint8_t* buf, size_t buflen, uint16_t hidden_views[PARAM_NB_EXECUTIONS]);

/***********************************************
 *             Hash for challenge              *
 ***********************************************/

static void hash_for_mpc_challenge_1(uint8_t challenge_hash[PARAM_DIGEST_SIZE], uint8_t seed_commitments[PARAM_NB_EXECUTIONS][PARAM_NB_PARTIES][PARAM_DIGEST_SIZE],
    const instance_t* inst, const uint8_t* salt, const uint8_t* message, size_t mlen)
{
    hash_context ctx;
    hash_init_prefix(&ctx, HASH_PREFIX_FIRST_CHALLENGE);
    if(inst != NULL)
        hash_update_instance(&ctx, inst);
    if(mlen > 0)
        hash_update(&ctx, message, mlen);
    hash_update(&ctx, salt, PARAM_SALT_SIZE);
    hash_update(&ctx, (uint8_t*) seed_commitments, PARAM_NB_EXECUTIONS*PARAM_NB_PARTIES*PARAM_DIGEST_SIZE);
    hash_final(&ctx, challenge_hash);
}

static void hash_for_mpc_challenge_2(uint8_t challenge_hash[PARAM_DIGEST_SIZE], const uint8_t mpc_challenge_hash_1[PARAM_DIGEST_SIZE], uint8_t hint_commitments[PARAM_NB_EXECUTIONS][PARAM_DIGEST_SIZE],
    const uint8_t* salt, const uint8_t* message, size_t mlen)
{
    hash_context ctx;
    hash_init_prefix(&ctx, HASH_PREFIX_SECOND_CHALLENGE);
    if(mlen > 0)
        hash_update(&ctx, message, mlen);
    hash_update(&ctx, salt, PARAM_SALT_SIZE);
    hash_update(&ctx, mpc_challenge_hash_1, PARAM_DIGEST_SIZE);
    hash_update(&ctx, (uint8_t*) hint_commitments, PARAM_NB_EXECUTIONS*PARAM_DIGEST_SIZE);
    hash_final(&ctx, challenge_hash);
}

static void hash_for_view_challenge(uint8_t challenge_hash[PARAM_DIGEST_SIZE], const uint8_t mpc_challenge_hash_2[PARAM_DIGEST_SIZE],
    vec_broadcast_t* broadcast[PARAM_NB_EXECUTIONS][PARAM_NB_PACKS], vec_broadcast_t* plain_broadcast[], 
    const uint8_t* salt, const uint8_t* message, size_t mlen)
{
    hash_context ctx;
    hash_init_prefix(&ctx, HASH_PREFIX_THIRD_CHALLENGE);
    if(mlen > 0)
        hash_update(&ctx, message, mlen);
    hash_update(&ctx, salt, PARAM_SALT_SIZE);
    hash_update(&ctx, mpc_challenge_hash_2, PARAM_DIGEST_SIZE);
    for (size_t e=0; e<PARAM_NB_EXECUTIONS; e++)
        for(size_t p=0; p<PARAM_NB_PACKS; p++)
            hash_update(&ctx, (uint8_t*) broadcast[e][p], PARAM_BR_SIZE);
    for (size_t e=0; e<PARAM_NB_EXECUTIONS; e++)
        hash_update(&ctx, (uint8_t*) plain_broadcast[e], PARAM_UNIF_SIZE); // We do not commit the v's zero values
    hash_final(&ctx, challenge_hash);
}

/***********************************************
 *             Signing Algorithm               *
 ***********************************************/

int mpcith_additive_based_7r_sign(uint8_t* sig, size_t* siglen,
                const uint8_t* m, size_t mlen,
                const uint8_t* sk,
                const uint8_t* salt, const uint8_t* seed
                ) {
    uint32_t i, j, e, p;
    int ret;

#ifndef NDEBUG
    // In debug mode, let us check if the key generation
    //    produces valid key pair. 
    ret = mpcith_validate_keys(NULL, sk);
    if(ret)
        printf("Error: SK invalid for signing.\n");
#endif

    // Deserialize the private key
    mpcith_secret_key_t ssk;
    ret = deserialize_secret_key(&ssk, sk, PARAM_SECRETKEYBYTES);
    if(ret < 0)
        return ret;
    uncompress_instance(ssk.inst);
    vec_wit_t* plain_wit = ssk.wit;

    // Signature Structure
    signature_additive_based_7r_t* ssig = init_signature_structure(salt, sig, PARAM_SIGNATURE_SIZEBYTES);

    /********************************************/
    /**********     INITIALIZATION     **********/
    /********************************************/

    prg_context entropy_ctx;
    samplable_t entropy = prg_to_samplable(&entropy_ctx);
    #ifdef PARAM_RND_EXPANSION_X4
    prg_context_x4 entropy_ctx_x4;
    samplable_x4_t entropy_x4 = prg_to_samplable_x4(&entropy_ctx_x4);
    #else /* PARAM_RND_EXPANSION_X4 */
    prg_context entropy_ctx_x4[4];
    samplable_t entropy_x4[4];
    for(j=0; j<4; j++)
        entropy_x4[j]= prg_to_samplable(&entropy_ctx_x4[j]);
    #endif /* PARAM_RND_EXPANSION_X4 */
    seed_tree_t* seeds_tree[PARAM_NB_EXECUTIONS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        seeds_tree[e] = malloc_seed_tree(PARAM_LOG_NB_PARTIES, PARAM_NB_PARTIES);
    uint8_t seed_commitments[PARAM_NB_EXECUTIONS][PARAM_NB_PARTIES][PARAM_DIGEST_SIZE];
    uint8_t hint_commitments[PARAM_NB_EXECUTIONS][PARAM_DIGEST_SIZE];

    // Derive the root seeds for all parallel executions
    uint8_t rseed[PARAM_NB_EXECUTIONS][PARAM_SEED_SIZE];
    prg_init(&entropy_ctx, seed, ssig->salt);
    byte_sample(&entropy, (uint8_t*) rseed, PARAM_NB_EXECUTIONS*PARAM_SEED_SIZE);

    // We manipulate here pointers
    //    to avoid & when using
    //    to be more consistant with plain_wit
    vec_share_t* packed_shares[PARAM_NB_EXECUTIONS][PARAM_NB_PACKS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        for(p=0; p<PARAM_NB_PACKS; p++)
            packed_shares[e][p] = new_share();
    vec_unif_t* plain_unif[PARAM_NB_EXECUTIONS];
    vec_hint_t* plain_hint[PARAM_NB_EXECUTIONS];

    /********************************************/
    /********   COMMIT PARTIES' INPUTS   ********/
    /********************************************/

    __BENCHMARK_START__(BS_SHARE_PREPARATION);
    vec_share_t* shares[PARAM_NB_PARTIES];
    for(i=0; i<PARAM_NB_PARTIES; i++)
        shares[i] = (i<4) ? new_share() : shares[i % 4];
    vec_share_t* acc = new_share();
    vec_unif_t* last_unif[PARAM_NB_EXECUTIONS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        last_unif[e] = new_unif();

    packing_context_t packing_ctx[PARAM_NB_EXECUTIONS];

    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        // Build the seed tree of the current execution
        __BENCHMARK_START__(BS_TREE_EXPANSION);
        expand_seed_tree(seeds_tree[e], rseed[e], ssig->salt);
        __BENCHMARK_STOP__(BS_TREE_EXPANSION);
        uint8_t** seeds = get_leaves(seeds_tree[e]);
        vec_share_setzero(acc);

        __BENCHMARK_START__(BS_SHARE_PACKING);
        packing_init(&packing_ctx[e], packed_shares[e]);
        __BENCHMARK_STOP__(BS_SHARE_PACKING);

        i=0;
        // Let us treat the parties four by four...
        for(; (i+3)+1<PARAM_NB_PARTIES; i+=4) {
            const uint8_t* ptr_seeds[4] = {seeds[i], seeds[i+1], seeds[i+2], seeds[i+3]};
            __BENCHMARK_START__(BS_RND_EXPANSION);
            #ifdef PARAM_RND_EXPANSION_X4
            prg_init_x4(&entropy_ctx_x4, ptr_seeds, NULL);
            vec_share_rnd_x4((vec_share_t* const*) &shares[i], &entropy_x4);
            #else /* PARAM_RND_EXPANSION_X4 */
            // No need of the salt, we do not care about
            //    collisions of the leave seeds
            prg_init_x4_array(entropy_ctx_x4, ptr_seeds, NULL);
            for(j=0; j<4; j++)
                vec_share_rnd(shares[i+j], &entropy_x4[j]);
            #endif /* PARAM_RND_EXPANSION_X4 */
            __BENCHMARK_STOP__(BS_RND_EXPANSION);
            for(j=0; j<4; j++) {
                vec_share_add(acc, shares[j]);
                __BENCHMARK_START__(BS_SHARE_PACKING);
                packing_update(&packing_ctx[e], i+j, shares[i+j]);
                __BENCHMARK_STOP__(BS_SHARE_PACKING);
            }
            __BENCHMARK_START__(BS_COMMITMENT);
            uint8_t* digests[4] = {
                seed_commitments[e][i+0], seed_commitments[e][i+1],
                seed_commitments[e][i+2], seed_commitments[e][i+3], 
            };
            const uint16_t is[4] = {(uint16_t)i, (uint16_t)(i+1), (uint16_t)(i+2), (uint16_t)(i+3)};
            commit_seed_x4(digests, (uint8_t const*const*) &seeds[i], ssig->salt, (uint16_t)e, is);
            __BENCHMARK_STOP__(BS_COMMITMENT);
        }
        // Now we treat the last parties
        for(;i<PARAM_NB_PARTIES; i++) {
            __BENCHMARK_START__(BS_RND_EXPANSION);
            // No need of the salt, we do not care about
            //    collisions of the leave seeds
            prg_init(&entropy_ctx, seeds[i], NULL);
            __BENCHMARK_STOP__(BS_RND_EXPANSION);
            if(i != PARAM_NB_PARTIES-1) {
                // Expand the input share from seed
                __BENCHMARK_START__(BS_RND_EXPANSION);
                vec_share_rnd(shares[i], &entropy);
                __BENCHMARK_STOP__(BS_RND_EXPANSION);
                vec_share_add(acc, shares[i]);

                // Aggregate to get the shares of the "main parties"
                __BENCHMARK_START__(BS_SHARE_PACKING);
                packing_update(&packing_ctx[e], i, shares[i]);
                __BENCHMARK_STOP__(BS_SHARE_PACKING);
            
                // Commit to the party's input (by committing to its seed)
                __BENCHMARK_START__(BS_COMMITMENT);
                commit_seed(seed_commitments[e][i], seeds[i], ssig->salt, (uint16_t)e, (uint16_t)i);
                __BENCHMARK_STOP__(BS_COMMITMENT);
            } else {
                // Compute plain unif
                __BENCHMARK_START__(BS_RND_EXPANSION);
                vec_unif_rnd(last_unif[e], &entropy);
                __BENCHMARK_STOP__(BS_RND_EXPANSION);
                plain_unif[e] = new_unif();
                vec_unif_set(plain_unif[e], last_unif[e]);
                vec_unif_add(plain_unif[e], get_unif(acc));
                vec_unif_normalize(plain_unif[e]);

                // For the moment, ssig->proofs[e].hint just contains the hint accumulator
                vec_hint_set(ssig->proofs[e].hint, get_hint(acc));

                // Compute aux
                vec_wit_set(ssig->proofs[e].wit, plain_wit);
                vec_wit_sub(ssig->proofs[e].wit, get_wit(acc));
                vec_wit_normalize(ssig->proofs[e].wit);

                // Commit to the party's input
                __BENCHMARK_START__(BS_COMMITMENT);
                commit_seed_and_wit(seed_commitments[e][i], seeds[i], ssig->proofs[e].wit, ssig->salt, (uint16_t)e, (uint16_t)i);
                __BENCHMARK_STOP__(BS_COMMITMENT);
            }
        }
    }
    free(acc);
    for(j=0; j<4; j++)
        free(shares[j]);
    __BENCHMARK_STOP__(BS_SHARE_PREPARATION);


    // Expand the MPC challenge N°1
    __BENCHMARK_START__(BS_CHALLENGE);
    vec_challenge_1_t* mpc_challenges_1[PARAM_NB_EXECUTIONS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        mpc_challenges_1[e] = new_challenge_1();
    hash_for_mpc_challenge_1(ssig->mpc_challenge_1_hash, seed_commitments, ssk.inst, ssig->salt, NULL, 0);
    expand_mpc_challenge_hash_1(mpc_challenges_1, ssig->mpc_challenge_1_hash, PARAM_NB_EXECUTIONS, ssk.inst);
    __BENCHMARK_STOP__(BS_CHALLENGE);

    /********************************************/
    /*********    COMPUTE THE HINTS    **********/
    /********************************************/

    __BENCHMARK_START__(B_PIN_D);
    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        plain_hint[e] = new_hint();
        compute_hint(plain_hint[e], plain_wit, plain_unif[e], ssk.inst, mpc_challenges_1[e]);
        vec_hint_neg(ssig->proofs[e].hint);
        vec_hint_add(ssig->proofs[e].hint, plain_hint[e]);
        commit_hint(hint_commitments[e], ssig->proofs[e].hint, ssig->salt, (uint16_t)e, PARAM_NB_PARTIES);
        memcpy(ssig->proofs[e].hint_digest, hint_commitments[e], PARAM_DIGEST_SIZE);

        __BENCHMARK_START__(BS_SHARE_PACKING);
        vec_share_t* last_share = new_share();
        vec_wit_set(get_wit(last_share), ssig->proofs[e].wit);
        vec_unif_set(get_unif(last_share), last_unif[e]);
        vec_hint_set(get_hint(last_share), ssig->proofs[e].hint);
        packing_update(&packing_ctx[e], PARAM_NB_PARTIES-1, last_share);
        packing_final(&packing_ctx[e]);
        __BENCHMARK_STOP__(BS_SHARE_PACKING);
        free(last_unif[e]);
    }
    __BENCHMARK_STOP__(B_PIN_D);

    // Expand the MPC challenge N°2
    __BENCHMARK_START__(BS_CHALLENGE);
    vec_challenge_2_t* mpc_challenges_2[PARAM_NB_EXECUTIONS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        mpc_challenges_2[e] = new_challenge_2();
    hash_for_mpc_challenge_2(ssig->mpc_challenge_2_hash, ssig->mpc_challenge_1_hash, hint_commitments, ssig->salt, NULL, 0);
    expand_mpc_challenge_hash_2(mpc_challenges_2, ssig->mpc_challenge_2_hash, PARAM_NB_EXECUTIONS, ssk.inst);
    __BENCHMARK_STOP__(BS_CHALLENGE);

    /********************************************/
    /*********  SIMULATE MPC PROTOCOL  **********/
    /********************************************/

    __BENCHMARK_START__(BS_MPC_EMULATION);
    vec_broadcast_t* plain_broadcast[PARAM_NB_EXECUTIONS];
    vec_broadcast_t* broadcast[PARAM_NB_EXECUTIONS][PARAM_NB_PACKS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        vec_share_t* plain = new_share();
        vec_wit_set(get_wit(plain), plain_wit);
        vec_unif_set(get_unif(plain), plain_unif[e]);
        vec_hint_set(get_hint(plain), plain_hint[e]);

        // Compute the values open by the parties
        plain_broadcast[e] = new_br();
        mpc_compute_plain_broadcast(plain_broadcast[e], mpc_challenges_1[e], mpc_challenges_2[e], plain, ssk.inst);
        vec_br_set(ssig->proofs[e].plain_broadcast, plain_broadcast[e]);
    
        // Compute the broadcast messages of the main parties
        for(p=0; p<PARAM_NB_PACKS; p++) {
            broadcast[e][p] = new_br();
            mpc_compute_communications(broadcast[e][p], mpc_challenges_1[e], mpc_challenges_2[e], packed_shares[e][p], ssig->proofs[e].plain_broadcast, ssk.inst, 0);
        }
        free(plain);
    }
    __BENCHMARK_STOP__(BS_MPC_EMULATION);

    // Expand the view challenge
    __BENCHMARK_START__(BS_CHALLENGE);
    uint16_t hidden_views[PARAM_NB_EXECUTIONS];
    hash_for_view_challenge(ssig->view_challenge_hash, ssig->mpc_challenge_2_hash, broadcast, plain_broadcast, ssig->salt, m, mlen);
    __BENCHMARK_STOP__(BS_CHALLENGE);
    expand_view_challenge_hash((uint16_t*) hidden_views, ssig->view_challenge_hash, PARAM_NB_EXECUTIONS, 1);

    /********************************************/
    /**********   FINALIZE SIGNATURE   **********/
    /********************************************/

    __BENCHMARK_START__(BS_BUILD_SIG);
    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        uint16_t num_unopened_party = hidden_views[e];
        ssig->proofs[e].len_seed_info = get_seed_path(ssig->proofs[e].seed_info, seeds_tree[e], num_unopened_party);
        memcpy(ssig->proofs[e].unopened_digest, seed_commitments[e][num_unopened_party], PARAM_DIGEST_SIZE);
    }

    // Serialize the signature
    ret = build_signature(ssig, sig, PARAM_SIGNATURE_SIZEBYTES, hidden_views);
    free_signature(ssig);
    __BENCHMARK_STOP__(BS_BUILD_SIG);

    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        free_seed_tree(seeds_tree[e]);
    mpcith_free_keys_internal(NULL, &ssk);
    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        for(p=0; p<PARAM_NB_PACKS; p++) {
            free(packed_shares[e][p]);
            free(broadcast[e][p]);
        }
        free(plain_unif[e]);
        free(plain_hint[e]);
        free(plain_broadcast[e]);
        free(mpc_challenges_1[e]);
        free(mpc_challenges_2[e]);
    }

    if(ret < 0)
        return ret;
    *siglen = (size_t) ret;
    return 0;
}

/***********************************************
 *            Verication Algorithm             *
 ***********************************************/

int mpcith_additive_based_7r_sign_verify(const uint8_t* sig, size_t siglen,
                const uint8_t* m, size_t mlen,
                const uint8_t* pk
                ) {
    uint32_t i, j, e, p;
    int ret;

    // Deserialize the public key
    mpcith_public_key_t ppk;
    ret = deserialize_public_key(&ppk, pk, PARAM_PUBLICKEYBYTES);
    if (ret < 0)
        return ret;
    uncompress_instance(ppk.inst);

    // Parse the signature
    // Note: while parsing, it expands the view challenge.
    uint16_t hidden_views[PARAM_NB_EXECUTIONS];
    const_signature_additive_based_7r_t* ssig = parse_signature(sig, siglen, hidden_views);
    if(ssig == NULL) {
        ret = -1;
        mpcith_free_keys_internal(&ppk, NULL);
        return ret;
    }

    // Initialize
    uint8_t seed_commitments[PARAM_NB_EXECUTIONS][PARAM_NB_PARTIES][PARAM_DIGEST_SIZE];
    uint8_t hint_commitments[PARAM_NB_EXECUTIONS][PARAM_DIGEST_SIZE];
    prg_context entropy_ctx;
    samplable_t entropy = prg_to_samplable(&entropy_ctx);
    #ifdef PARAM_RND_EXPANSION_X4
    prg_context_x4 entropy_ctx_x4;
    samplable_x4_t entropy_x4 = prg_to_samplable_x4(&entropy_ctx_x4);
    #else /* PARAM_RND_EXPANSION_X4 */
    prg_context entropy_ctx_x4[4];
    samplable_t entropy_x4[4];
    for(j=0; j<4; j++)
        entropy_x4[j]= prg_to_samplable(&entropy_ctx_x4[j]);
    #endif /* PARAM_RND_EXPANSION_X4 */

    // Expand MPC Challenge
    __BENCHMARK_START__(BV_CHALLENGE);
    vec_challenge_1_t* mpc_challenges_1[PARAM_NB_EXECUTIONS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        mpc_challenges_1[e] = new_challenge_1();
    expand_mpc_challenge_hash_1(mpc_challenges_1, ssig->mpc_challenge_1_hash, PARAM_NB_EXECUTIONS, ppk.inst);

    vec_challenge_2_t* mpc_challenges_2[PARAM_NB_EXECUTIONS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++)
        mpc_challenges_2[e] = new_challenge_2();
    expand_mpc_challenge_hash_2(mpc_challenges_2, ssig->mpc_challenge_2_hash, PARAM_NB_EXECUTIONS, ppk.inst);
    __BENCHMARK_STOP__(BV_CHALLENGE);

    vec_share_t* shares[PARAM_NB_PARTIES];
    for(i=0; i<PARAM_NB_PARTIES; i++)
        shares[i] = (i<4) ? new_share() : shares[i % 4];
    vec_share_t* party_shares[PARAM_NB_PACKS];
    for(p=0; p<PARAM_NB_PACKS; p++)
        party_shares[p] = new_share();
    vec_broadcast_t* plain_broadcast[PARAM_NB_EXECUTIONS];
    vec_broadcast_t* broadcast[PARAM_NB_EXECUTIONS][PARAM_NB_PACKS];
    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        plain_broadcast[e] = new_br();
        for(p=0; p<PARAM_NB_PACKS; p++)
            broadcast[e][p] = new_br();
    }
    share_recomputing_context_t packing_ctx;

    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        // Get the open leaf seeds
        __BENCHMARK_START__(BV_TREE_EXPANSION);
        seed_tree_t* seed_tree = malloc_seed_tree(PARAM_LOG_NB_PARTIES, PARAM_NB_PARTIES);
        reconstruct_tree(seed_tree, hidden_views[e], ssig->proofs[e].seed_info, ssig->salt);
        uint8_t** seeds = get_leaves(seed_tree);
        __BENCHMARK_STOP__(BV_TREE_EXPANSION);

        // Get the commitment of the hint
        if(hidden_views[e] == PARAM_NB_PARTIES-1)
            memcpy(hint_commitments[e], ssig->proofs[e].hint_digest, PARAM_DIGEST_SIZE);
        else
            commit_hint(hint_commitments[e], ssig->proofs[e].hint, ssig->salt, (uint16_t)e, PARAM_NB_PARTIES);

        // Get the plain broadcast
        vec_br_set(plain_broadcast[e], ssig->proofs[e].plain_broadcast);

        // Get the share of the main parties
        __BENCHMARK_START__(BV_SHARE_PACKING);
        share_recomputing_init(&packing_ctx, hidden_views[e], party_shares);
        __BENCHMARK_STOP__(BV_SHARE_PACKING);

        i=0;
        // Let us treat the parties four by four...
        for(; (i+3)+1<PARAM_NB_PARTIES; i+=4) {
            __BENCHMARK_START__(BV_COMMITMENT);
            uint8_t* digests[4] = {
                seed_commitments[e][i+0], seed_commitments[e][i+1],
                seed_commitments[e][i+2], seed_commitments[e][i+3], 
            };
            const uint16_t is[4] = {(uint16_t)i, (uint16_t)(i+1), (uint16_t)(i+2), (uint16_t)(i+3)};
            commit_seed_x4(digests, (uint8_t const*const*) &seeds[i], ssig->salt, (uint16_t)e, is);
            __BENCHMARK_STOP__(BV_COMMITMENT);
            const uint8_t* ptr_seeds[4] = {seeds[i], seeds[i+1], seeds[i+2], seeds[i+3]};
            __BENCHMARK_START__(BV_RND_EXPANSION);
            #ifdef PARAM_RND_EXPANSION_X4
            prg_init_x4(&entropy_ctx_x4, ptr_seeds, NULL);
            vec_share_rnd_x4((vec_share_t* const*) &shares[i], &entropy_x4);
            #else /* PARAM_RND_EXPANSION_X4 */
            // No need of the salt, we do not care about
            //    collisions of the leave seeds
            prg_init_x4_array(entropy_ctx_x4, ptr_seeds, NULL);
            for(j=0; j<4; j++)
                vec_share_rnd(shares[i+j], &entropy_x4[j]);
            #endif /* PARAM_RND_EXPANSION_X4 */
            __BENCHMARK_STOP__(BV_RND_EXPANSION);
            for(j=0; j<4; j++) {
                if(i+j != hidden_views[e]) {
                    __BENCHMARK_START__(BV_SHARE_PACKING);
                    share_recomputing_update(&packing_ctx, i+j, shares[i+j]);
                    __BENCHMARK_STOP__(BV_SHARE_PACKING);
                } else {
                    memcpy(seed_commitments[e][i+j], ssig->proofs[e].unopened_digest, PARAM_DIGEST_SIZE);
                }
            }
        }
        // Now we treat the last parties
        for(; i<PARAM_NB_PARTIES; i++) {
            if(i == hidden_views[e]) {
                memcpy(seed_commitments[e][i], ssig->proofs[e].unopened_digest, PARAM_DIGEST_SIZE);
                continue;
            }
            prg_init(&entropy_ctx, seeds[i], NULL);
            if(i != PARAM_NB_PARTIES-1) {
                // Expand the input share from seed
                __BENCHMARK_START__(BV_RND_EXPANSION);
                vec_share_rnd(shares[i], &entropy);
                __BENCHMARK_STOP__(BV_RND_EXPANSION);

                // Recompute the party's commitment
                __BENCHMARK_START__(BV_COMMITMENT);
                commit_seed(seed_commitments[e][i], seeds[i], ssig->salt, (uint16_t)e, (uint16_t)i);
                __BENCHMARK_STOP__(BV_COMMITMENT);
            } else {
                // Get the party's input share
                __BENCHMARK_START__(BV_RND_EXPANSION);
                vec_unif_rnd(get_unif(shares[i]), &entropy);
                __BENCHMARK_STOP__(BV_RND_EXPANSION);
                vec_wit_set(get_wit(shares[i]), ssig->proofs[e].wit);
                vec_hint_set(get_hint(shares[i]), ssig->proofs[e].hint);

                // Recompute the party's commitment
                __BENCHMARK_START__(BV_COMMITMENT);
                commit_seed_and_wit(seed_commitments[e][i], seeds[i], ssig->proofs[e].wit, ssig->salt, (uint16_t)e, (uint16_t)i);
                __BENCHMARK_STOP__(BV_COMMITMENT);
            }

            __BENCHMARK_START__(BV_SHARE_PACKING);
            share_recomputing_update(&packing_ctx, i, shares[i]);
            __BENCHMARK_STOP__(BV_SHARE_PACKING);
        }
        __BENCHMARK_START__(BV_SHARE_PACKING);
        share_recomputing_final(&packing_ctx);
        __BENCHMARK_STOP__(BV_SHARE_PACKING);

        // Re-emulate the computation of the open main parties
        __BENCHMARK_START__(BV_MPC_EMULATION);
        for(p=0; p<PARAM_NB_PACKS; p++)
            mpc_compute_communications(broadcast[e][p], mpc_challenges_1[e], mpc_challenges_2[e], party_shares[p], ssig->proofs[e].plain_broadcast, ppk.inst, has_sharing_offset_for(hidden_views[e],p));
        
        recompose_broadcast(broadcast[e], ssig->proofs[e].plain_broadcast, hidden_views[e]);
        __BENCHMARK_STOP__(BV_MPC_EMULATION);

        free_seed_tree(seed_tree);
    }
    for(j=0; j<4; j++)
        free(shares[j]);
    for(p=0; p<PARAM_NB_PACKS; p++)
        free(party_shares[p]);

    // Recompute the hash digests of the challenges
    //    and check they are consistent with the ones in the signature
    uint8_t mpc_challenge_1_hash[PARAM_DIGEST_SIZE], mpc_challenge_2_hash[PARAM_DIGEST_SIZE], view_challenge_hash[PARAM_DIGEST_SIZE];
    hash_for_mpc_challenge_1(mpc_challenge_1_hash, seed_commitments, ppk.inst, ssig->salt, NULL, 0);
    hash_for_mpc_challenge_2(mpc_challenge_2_hash, ssig->mpc_challenge_1_hash, hint_commitments, ssig->salt, NULL, 0);
    hash_for_view_challenge(view_challenge_hash, ssig->mpc_challenge_2_hash, broadcast, plain_broadcast, ssig->salt, m, mlen);
    ret = (memcmp(mpc_challenge_1_hash, ssig->mpc_challenge_1_hash, PARAM_DIGEST_SIZE) != 0);
    ret |= (memcmp(mpc_challenge_2_hash, ssig->mpc_challenge_2_hash, PARAM_DIGEST_SIZE) != 0);
    ret |= (memcmp(view_challenge_hash, ssig->view_challenge_hash, PARAM_DIGEST_SIZE) != 0);

    mpcith_free_keys_internal(&ppk, NULL);
    free_const_signature(ssig);
    for(e=0; e<PARAM_NB_EXECUTIONS; e++) {
        for(p=0; p<PARAM_NB_PACKS; p++)
            free(broadcast[e][p]);
        free(plain_broadcast[e]);
        free(mpc_challenges_1[e]);
        free(mpc_challenges_2[e]);
    }
    return ret;
}

/***********************************************
 *             About Serialization             *
 ***********************************************/

void free_signature(signature_additive_based_7r_t* sig) {
    if(sig->allocated_memory != NULL)
        free(sig->allocated_memory);
    free(sig);
}

void free_const_signature(const_signature_additive_based_7r_t* sig) {
    if(sig->allocated_memory != NULL)
        free(sig->allocated_memory);
    free(sig);
}

const_signature_additive_based_7r_t* parse_signature(const uint8_t* buf, size_t buflen, uint16_t hidden_views[PARAM_NB_EXECUTIONS]) {
    size_t bytes_required = 3*PARAM_DIGEST_SIZE + PARAM_SALT_SIZE;
    if(buflen < bytes_required) {
        return NULL;
    }

    const_signature_additive_based_7r_t* sig = (const_signature_additive_based_7r_t*)malloc(sizeof(const_signature_additive_based_7r_t));
    sig->salt = buf;                  buf += PARAM_SALT_SIZE;
    sig->mpc_challenge_1_hash = buf;  buf += PARAM_DIGEST_SIZE;
    sig->mpc_challenge_2_hash = buf;  buf += PARAM_DIGEST_SIZE;
    sig->view_challenge_hash = buf;   buf += PARAM_DIGEST_SIZE;
    expand_view_challenge_hash((uint16_t*) hidden_views, sig->view_challenge_hash, PARAM_NB_EXECUTIONS, 1);

    for(size_t e=0; e<PARAM_NB_EXECUTIONS; e++) {
        sig->proofs[e].len_seed_info = get_seed_path_size(PARAM_LOG_NB_PARTIES, PARAM_NB_PARTIES, hidden_views[e]);
        bytes_required += sig->proofs[e].len_seed_info + PARAM_DIGEST_SIZE + PARAM_COMPRESSED_BR_SIZE;
        if(hidden_views[e] != PARAM_NB_PARTIES-1)
            bytes_required += PARAM_WIT_SHORT_SIZE + PARAM_HINT_SHORT_SIZE;
        else
            bytes_required += PARAM_DIGEST_SIZE;
    }
    if(buflen != bytes_required) {
        free(sig);
        return NULL;
    }

    sig->allocated_memory = (uint8_t*)malloc(
        PARAM_NB_EXECUTIONS*(
            PARAM_BR_SIZE+PARAM_WIT_PARSED_SIZE+PARAM_HINT_PARSED_SIZE
        )
    );
    uint8_t* buf_ = sig->allocated_memory;
    for(size_t e=0; e<PARAM_NB_EXECUTIONS; e++) {
        sig->proofs[e].unopened_digest = buf;
        buf += PARAM_DIGEST_SIZE;
        sig->proofs[e].plain_broadcast = (vec_broadcast_t*) buf_;
        buf_ += PARAM_BR_SIZE;
        uncompress_plain_broadcast(sig->proofs[e].plain_broadcast, buf);
        buf += PARAM_COMPRESSED_BR_SIZE;
        sig->proofs[e].seed_info = buf;
        buf += sig->proofs[e].len_seed_info;
        if(hidden_views[e] != PARAM_NB_PARTIES-1) {
            wit_deserialize(&sig->proofs[e].wit, buf, buf_);
            buf += PARAM_WIT_SHORT_SIZE;
            buf_ += PARAM_WIT_PARSED_SIZE;
            hint_deserialize(&sig->proofs[e].hint, buf, buf_);
            buf += PARAM_HINT_SHORT_SIZE;
            buf_ += PARAM_HINT_PARSED_SIZE;
        } else {
            sig->proofs[e].hint_digest = buf;
            buf += PARAM_DIGEST_SIZE;
        }
    }

    return sig;
}

signature_additive_based_7r_t* init_signature_structure(const uint8_t* salt, uint8_t* buf, size_t buflen) {
    size_t bytes_required = 3*PARAM_DIGEST_SIZE + PARAM_SALT_SIZE;
    if(buflen < bytes_required) {
        return NULL;
    }

    signature_additive_based_7r_t* sig = (signature_additive_based_7r_t*)malloc(sizeof(signature_additive_based_7r_t));
    sig->salt = buf;               buf += PARAM_SALT_SIZE;
    sig->mpc_challenge_1_hash = buf;   buf += PARAM_DIGEST_SIZE;
    sig->mpc_challenge_2_hash = buf;   buf += PARAM_DIGEST_SIZE;
    sig->view_challenge_hash = buf;  buf += PARAM_DIGEST_SIZE;
    memcpy(sig->salt, salt, PARAM_SALT_SIZE);

    sig->allocated_memory = (uint8_t*) malloc(
        PARAM_NB_EXECUTIONS*(
            PARAM_LOG_NB_PARTIES*PARAM_SEED_SIZE // seed tree
             + PARAM_DIGEST_SIZE // unopen commitment
             + PARAM_BR_SIZE // plain broadcast
             + PARAM_WIT_SIZE // wit of last party
             + PARAM_HINT_SIZE // hint of last party
             + PARAM_DIGEST_SIZE // hint commitment
        )
    );
    uint8_t* buf_ = sig->allocated_memory;
    for(size_t e=0; e<PARAM_NB_EXECUTIONS; e++) {
        sig->proofs[e].unopened_digest = buf_;
        buf_ += PARAM_DIGEST_SIZE;
        sig->proofs[e].plain_broadcast = (vec_broadcast_t*) buf_;
        buf_ += PARAM_BR_SIZE;
        sig->proofs[e].seed_info = buf_;
        buf_ += PARAM_LOG_NB_PARTIES*PARAM_SEED_SIZE;
        sig->proofs[e].wit = (vec_wit_t*) buf_;   buf_ += PARAM_WIT_SIZE;
        sig->proofs[e].hint = (vec_hint_t*) buf_;  buf_ += PARAM_HINT_SIZE;
        sig->proofs[e].hint_digest = buf_;   buf_ += PARAM_DIGEST_SIZE;
    }

    return sig;
}

int build_signature(const signature_additive_based_7r_t* sig, uint8_t* buf, size_t buflen, uint16_t hidden_views[PARAM_NB_EXECUTIONS]) {
    size_t bytes_required = (3*PARAM_DIGEST_SIZE + PARAM_SALT_SIZE);
    bytes_required += PARAM_NB_EXECUTIONS*PARAM_COMPRESSED_BR_SIZE;
    bytes_required += PARAM_NB_EXECUTIONS*PARAM_DIGEST_SIZE;
    for(size_t e=0; e<PARAM_NB_EXECUTIONS; e++) {
        bytes_required += sig->proofs[e].len_seed_info;
        if(hidden_views[e] != PARAM_NB_PARTIES-1)
            bytes_required += PARAM_WIT_SHORT_SIZE + PARAM_HINT_SHORT_SIZE;
        else
            bytes_required += PARAM_DIGEST_SIZE;
    }
    if(buflen < bytes_required)
        return -1;

    uint8_t* bufBase = buf;
    buf += (3*PARAM_DIGEST_SIZE + PARAM_SALT_SIZE); // salt and challenge hashes are already set (via init)
    for(size_t e=0; e<PARAM_NB_EXECUTIONS; e++) {
        memcpy(buf, sig->proofs[e].unopened_digest, PARAM_DIGEST_SIZE);
        buf += PARAM_DIGEST_SIZE;
        compress_plain_broadcast(buf, sig->proofs[e].plain_broadcast);
        buf += PARAM_COMPRESSED_BR_SIZE;
        memcpy(buf, sig->proofs[e].seed_info, sig->proofs[e].len_seed_info);
        buf += sig->proofs[e].len_seed_info;
        if(hidden_views[e] != PARAM_NB_PARTIES-1) {
            wit_serialize(buf, sig->proofs[e].wit);
            buf += PARAM_WIT_SHORT_SIZE;
            hint_serialize(buf, sig->proofs[e].hint);
            buf += PARAM_HINT_SHORT_SIZE;
        } else {
            memcpy(buf, sig->proofs[e].hint_digest, PARAM_DIGEST_SIZE);
            buf += PARAM_DIGEST_SIZE;
        }
    }

    return (int)(buf - bufBase);
}
