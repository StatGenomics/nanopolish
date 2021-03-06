//---------------------------------------------------------
// Copyright 2015 Ontario Institute for Cancer Research
// Written by Jared Simpson (jared.simpson@oicr.on.ca)
//---------------------------------------------------------
//
// nanopolish_profile_hmm -- Profile Hidden Markov Model
//
#include <algorithm>
#include "nanopolish_profile_hmm.h"

//#define DEBUG_FILL
//#define PRINT_TRAINING_MESSAGES 1

void profile_hmm_forward_initialize(FloatMatrix& fm)
{
    // initialize forward calculation
    for(uint32_t si = 0; si < fm.n_cols; si++) {
        set(fm, 0, si, -INFINITY);
    }

    for(uint32_t ri = 0; ri < fm.n_rows; ri++) {
        set(fm, ri, PS_KMER_SKIP, -INFINITY);
        set(fm, ri, PS_EVENT_SPLIT, -INFINITY);
        set(fm, ri, PS_MATCH, -INFINITY);
    }
}

// Terminate the forward algorithm by calculating
// the probability of transitioning to the end state
// for all columns and a given row
float profile_hmm_forward_terminate(const FloatMatrix&,
                                    const FloatMatrix&,
                                    uint32_t)
{
    assert(false);
    return -INFINITY;

    /*
    float sum = -INFINITY;
    uint32_t tcol = fm.n_cols - 1;
    for(uint32_t sk = 0; sk < fm.n_cols - 1; sk++) {
        // transition probability from state k to state l
        float t_kl = get(tm, sk, tcol);
        float fm_k = get(fm, row, sk);
        sum = add_logs(sum, t_kl + fm_k);
    }
    return sum;
    */
}

// convenience function to run the HMM over multiple inputs and sum the result
float profile_hmm_score(const HMMInputSequence& sequence, const std::vector<HMMInputData>& data, const uint32_t flags)
{
    float score = 0.0f;
    for(size_t i = 0; i < data.size(); ++i) {
        score += profile_hmm_score(sequence, data[i], flags);
    }
    return score;
}

float profile_hmm_score(const HMMInputSequence& sequence, const HMMInputData& data, const uint32_t flags)
{
    const uint32_t k = data.read->pore_model[data.strand].k;
    uint32_t n_kmers = sequence.length() - k + 1;

    uint32_t n_states = PS_NUM_STATES * (n_kmers + 2); // + 2 for explicit terminal states

    uint32_t e_start = data.event_start_idx;
    uint32_t e_end = data.event_stop_idx;
    uint32_t n_events = 0;
    if(e_end > e_start)
        n_events = e_end - e_start + 1;
    else
        n_events = e_start - e_end + 1;

    uint32_t n_rows = n_events + 1;

    // Allocate a matrix to hold the HMM result
    FloatMatrix fm;
    allocate_matrix(fm, n_rows, n_states);

    profile_hmm_forward_initialize(fm);

    ProfileHMMForwardOutput output(&fm);

    float score = profile_hmm_fill_generic(sequence, data, e_start, flags, output);

    // cleanup
    free_matrix(fm);
    return score;
}

void profile_hmm_viterbi_initialize(FloatMatrix& m)
{
    // Same as forward initialization
    profile_hmm_forward_initialize(m);
}

std::vector<HMMAlignmentState> profile_hmm_align(const HMMInputSequence& sequence, const HMMInputData& data, const uint32_t flags)
{
    std::vector<HMMAlignmentState> alignment;
    const uint32_t k = data.read->pore_model[data.strand].k;

    uint32_t n_kmers = sequence.length() - k + 1;
    uint32_t n_states = PS_NUM_STATES * (n_kmers + 2); // + 2 for explicit terminal states

    uint32_t e_start = data.event_start_idx;
    uint32_t e_end = data.event_stop_idx;
    uint32_t n_events = 0;
    if(e_end > e_start)
        n_events = e_end - e_start + 1;
    else
        n_events = e_start - e_end + 1;
    assert(n_events >= 2);

    uint32_t n_rows = n_events + 1;
    
    // Allocate matrices to hold the HMM result
    FloatMatrix vm;
    allocate_matrix(vm, n_rows, n_states);
    
    UInt8Matrix bm;
    allocate_matrix(bm, n_rows, n_states);

    ProfileHMMViterbiOutput output(&vm, &bm);

    profile_hmm_viterbi_initialize(vm);
    profile_hmm_fill_generic(sequence, data, e_start, flags, output);

    // Traverse the backtrack matrix to compute the results
    int traversal_stride = data.event_stride;

#if HMM_REVERSE_FIX
    // Hack to support the fixed HMM
    // TODO: clean up
    traversal_stride = 1;
    if(data.event_stride == -1) {
        e_start = data.event_stop_idx;
    }
#endif
    
    // start from the last event matched to the last kmer
    uint32_t row = n_rows - 1;
    uint32_t col = PS_NUM_STATES * n_kmers + PS_MATCH;

    while(row > 0) {
        
        uint32_t event_idx = e_start + (row - 1) * traversal_stride;
        uint32_t block = col / PS_NUM_STATES;
        assert(block > 0);
        assert(get(vm, row, col) != -INFINITY);

        uint32_t kmer_idx = block - 1;
        
        ProfileState curr_ps = (ProfileState) (col % PS_NUM_STATES);

        HMMAlignmentState as;
        as.event_idx = event_idx;
        as.kmer_idx = kmer_idx;
        as.l_posterior = -INFINITY; // not computed
        as.l_fm = get(vm, row, col);
        as.log_transition_probability = -INFINITY; // not computed
        as.state = ps2char(curr_ps);
        alignment.push_back(as);

        // Update the event (row) and k-mer using the current state
        // The next state is encoded in the backtrack matrix for the current cell
        ProfileState next_ps = (ProfileState)get(bm, row, col);
        
        // If we hit the softclip state we are done aligning
        if(next_ps == PS_PRE_SOFT) {
            break;
        }

#if DEBUG_BACKTRACK
        printf("Backtrack [%zu %zu] k: %zu block: %zu curr_ps: %c next_ps: %c\n", row, col, kmer_idx, block, ps2char(curr_ps), ps2char(next_ps));
#endif

        if(curr_ps == PS_MATCH) {
            row -= 1;
            kmer_idx -= 1;
        } else if(curr_ps == PS_EVENT_SPLIT) {
            row -= 1;
            // kmer stays the same
        } else {
            assert(curr_ps == PS_KMER_SKIP);
            // row stays the same
            kmer_idx -= 1;
        }

        col = PS_NUM_STATES * (kmer_idx + 1) + next_ps;
    }


#if HMM_REVERSE_FIX
    // change the strand of the kmer indices if we aligned to the reverse strand
    if(data.event_stride == -1) {
        for(size_t ai = 0; ai < alignment.size(); ++ai) {
            size_t k_idx = alignment[ai].kmer_idx;
            alignment[ai].kmer_idx = sequence.length() - k_idx - k;
        }
    } else {
        std::reverse(alignment.begin(), alignment.end());
    }
#else
    //
    std::reverse(alignment.begin(), alignment.end());
#endif

    //
    free_matrix(vm);
    free_matrix(bm);

    return alignment;
}

// Print the alignment between the read-strand and a sequence
void print_alignment(const std::string& name,
                     uint32_t seq_id,
                     uint32_t read_id,
                     const HMMInputSequence& sequence, 
                     const HMMInputData& data,
                     const std::vector<HMMAlignmentState>& alignment)
{
    size_t n_matches = 0;
    size_t n_merges = 0;
    size_t n_skips = 0;
    size_t n_mergeskips = 0;
    
    const uint32_t k = data.read->pore_model[data.strand].k;

    char prev_s = '\0';
    for(size_t pi = 0; pi < alignment.size(); ++pi) {

        uint32_t ei = alignment[pi].event_idx;
        uint32_t ki = alignment[pi].kmer_idx;
        char s = alignment[pi].state;
    
        double level = data.read->get_drift_corrected_level(ei, data.strand);
        double sd = data.read->events[data.strand][ei].stdv;
        double duration = data.read->get_duration(ei, data.strand);
        uint32_t rank = sequence.get_kmer_rank(ki, k, data.rc);
        
        const PoreModel& pm = data.read->pore_model[data.strand];
        GaussianParameters model = pm.get_scaled_parameters(rank);

        double norm_level = (level - model.mean) / model.stdv;
        
        double model_sd_mean = 0.0f;
        double model_sd_stdv = 0.0f;

        n_matches += (s == 'M');
        n_merges += (s == 'E');
        n_skips += (s == 'K');
        n_mergeskips += (s == 'K' && prev_s == 'E');

        double lp_diff = 0.0f;
        if(pi > 0) {
            lp_diff = alignment[pi].l_fm - alignment[pi - 1].l_fm;
        } else {
            lp_diff = alignment[pi].l_fm;
        }
        std::string kmer = sequence.get_kmer(ki, k, false);
 
        printf("DEBUG\t%s\t%d\t%d\t%c\t", name.c_str(), read_id, data.rc, "tc"[data.strand]);
        printf("%c\t%d\t%d\t", s, ei, ki);
        printf("%s\t%.3lf\t", kmer.c_str(), duration);
        printf("%.1lf\t%.1lf\t%.1lf\t", level, model.mean, norm_level);
        printf("\t%.1lf\t%.1lf\t%.1lf\t", sd, model_sd_mean, (sd - model_sd_mean) / model_sd_stdv);
        printf("%.2lf\t%.2lf\t%.2lf\n", exp(alignment[pi].l_posterior), alignment[pi].l_fm, lp_diff);
        prev_s = s;
    }

    // Summarize alignment
    double time_start = data.read->events[data.strand][data.event_start_idx].start_time;
    double time_end = data.read->events[data.strand][data.event_stop_idx].start_time;
    double total_duration = fabs(time_start - time_end);
    double num_events = abs(data.event_start_idx - data.event_stop_idx) + 1;
    double final_lp = alignment[alignment.size() - 1].l_fm;
    double mean_lp = final_lp / num_events;

    // Print summary header on first entry
    static int once = 1;
    if(once) {
        printf("SUMMARY\tseq_name\tseq_id\tread_id\tis_rc\tstrand\t");
        printf("lp\tmean_lp\tnum_events\t");
        printf("n_matches\tn_merges\tn_skips\tn_mergeskips\ttotal_duration\n");
        once = 0;
    }

    printf("SUMMARY\t%s\t%d\t%d\t%d\t%c\t", name.c_str(), seq_id, read_id, data.rc, data.strand ? 't' : 'c');
    printf("%.2lf\t%.2lf\t%.0lf\t", final_lp, mean_lp, num_events);
    printf("%zu\t%zu\t%zu\t%zu\t%.2lf\n", n_matches, n_merges, n_skips, n_mergeskips, total_duration);
}
