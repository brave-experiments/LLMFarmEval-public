#include "../spm-headers/replit.h"
#include "../gpt_helpers.h"
#include "../spm-headers/gpt_spm.h"

#include "../ggml/ggml_dadbed9.h"

#include "../ggml/common.h"
#include "../ggml/common-ggml.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cinttypes>

#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using piece_t = std::pair<std::size_t, float>;
using piece_map_t = std::unordered_map<std::string, piece_t>;

struct replit_tokenizer {
    gpt_vocab raw_vocab;
    piece_map_t piece_map;
    std::vector<std::string> vocab;
};

std::pair<std::vector<std::size_t>, float> encode_word(const std::string & word, const piece_map_t & model) {
    std::vector<int> best_segmentations_starts(word.length() + 1, -1);
    best_segmentations_starts[0] = 0;

    std::vector<float> best_segmentations_scores(word.length() + 1, -std::numeric_limits<float>::infinity());
    best_segmentations_scores[0] = 1.0;

    for (int start_idx = 0; start_idx < word.length(); ++start_idx) {
        float best_score_at_start = best_segmentations_scores[start_idx];
        for (int end_idx = start_idx + 1; end_idx <= word.length(); ++end_idx) {
            std::string token = word.substr(start_idx, end_idx - start_idx);
            if (model.count(token) && best_score_at_start != -std::numeric_limits<float>::infinity()) {
                float token_score = model.at(token).second;
                float score = token_score + best_score_at_start;
                if (best_segmentations_scores[end_idx] == -std::numeric_limits<float>::infinity() ||
                    best_segmentations_scores[end_idx] > score) {
                    best_segmentations_starts[end_idx] = start_idx;
                    best_segmentations_scores[end_idx] = score;
                }
            }
        }
    }

    if (best_segmentations_scores.back() == -std::numeric_limits<float>::infinity()) {
        return std::make_pair(std::vector<std::size_t>{0}, 0.0f);
    }

    float score = best_segmentations_scores.back();
    int start = best_segmentations_starts.back();
    int end = word.length();
    std::vector<std::size_t> tokens;
    while (start != 0) {
        const auto token_id = model.at(word.substr(start, end - start)).first;
        tokens.insert(tokens.begin(), token_id);
        int next_start = best_segmentations_starts[start];
        end = start;
        start = next_start;
    }
    const auto token_id = model.at(word.substr(start, end - start)).first;
    tokens.insert(tokens.begin(), token_id);
    return std::make_pair(tokens, score);
}

bool replit_tokenizer_load(replit_tokenizer & tokenizer, std::istream & fin, int max_vocab_size) {
    std::string word;
    std::vector<char> buf(128);

    for (std::size_t i = 0; i < max_vocab_size; i++) {
        uint32_t len;
        fin.read((char *)&len, sizeof(len));

        buf.resize(len);
        fin.read((char *)buf.data(), len);
        word.assign(buf.data(), len);

        float score;
        fin.read((char *)&score, sizeof(score));

        tokenizer.piece_map[word] = std::make_pair(i, -score);
        tokenizer.raw_vocab.id_to_token[i] = word;
    }

    return true;
}

std::string replace_all(const std::string & str,    // where to work
                        const std::string & find,   // substitute 'find'
                        const std::string & replace //      by 'replace'
) {
    using namespace std;
    string result;
    size_t find_len = find.size();
    size_t pos, from = 0;
    while (string::npos != (pos = str.find(find, from))) {
        result.append(str, from, pos - from);
        result.append(replace);
        from = pos + find_len;
    }
    result.append(str, from, string::npos);
    return result;
}

std::string ws_symbol = "\342\226\201";
std::vector<std::size_t> replit_tokenizer_tokenize(replit_tokenizer & tokenizer, const std::string & text) {
    std::vector<std::size_t> tokens;
    auto normalized_text = replace_all(text, " ", ws_symbol);
    auto tokenized = encode_word(normalized_text, tokenizer.piece_map);

    return tokenized.first;
}

std::string replit_tokenizer_detokenize(replit_tokenizer & tokenizer, const std::vector<std::size_t> & tokens) {
    std::string text;
    for (auto token : tokens) {
        text += tokenizer.raw_vocab.id_to_token[token];
    }
    auto denormalized_text = replace_all(text, ws_symbol, " ");
    return denormalized_text;
}

//// no defaults for now
//struct mpt_hparams {
//    int32_t d_model     = 0;
//    int32_t max_seq_len = 0;
//    int32_t n_heads     = 0;
//    int32_t n_layers    = 0;
//    int32_t n_vocab     = 0;
//    int32_t ftype       = 0;
//};

struct replit_hparams:gpt_base_hparams {
    int32_t d_model = 0;
    int32_t max_seq_len = 0;
    int32_t n_heads = 0;
    int32_t n_layers = 0;
    int32_t n_vocab = 0;
    int32_t ftype = 0;
};


struct replit_layer {
    // pre normalization
    struct ggml_dadbed9_tensor * norm_1_weight;

    // attention
    struct ggml_dadbed9_tensor * c_attn_wqkv_weight;
    struct ggml_dadbed9_tensor * c_attn_out_proj_weight;

    // post normalization
    struct ggml_dadbed9_tensor * norm_2_weight;

    // ff
    struct ggml_dadbed9_tensor * ffn_up_proj;
    struct ggml_dadbed9_tensor * ffn_down_proj;
};

struct replit_model:gpt_base_model {
    replit_hparams hparams;
    struct ggml_dadbed9_tensor * wte_weight;    // position embedding
    struct ggml_dadbed9_tensor * norm_f_weight; // language model head
    std::vector<replit_layer> layers;
};


struct replit_context:gpt_base_context {
    replit_model model;
    replit_tokenizer vocab;
};

void replit_free(struct replit_context * ctx) {
    delete ctx;
}
// load the model's weights from a file
bool replit_model_load(const std::string & fname, replit_model & model, replit_tokenizer & vocab) {
    printf("%s: loading model from '%s' - please wait ...\n", __func__, fname.c_str());

    auto fin = std::ifstream(fname, std::ios::binary);
    if (!fin) {
        fprintf(stderr, "%s: failed to open '%s'\n", __func__, fname.c_str());
        return false;
    }

    // verify magic
    {
        uint32_t magic;
        fin.read((char *)&magic, sizeof(magic));
        if (magic != 0x67676d6c) {
            fprintf(stderr, "%s: invalid model file '%s' (bad magic)\n", __func__, fname.c_str());
            return false;
        }
    }

    // load hparams
    {
        auto & hparams = model.hparams;

        fin.read((char *)&hparams.d_model, sizeof(hparams.d_model));
        fin.read((char *)&hparams.max_seq_len, sizeof(hparams.max_seq_len));
        fin.read((char *)&hparams.n_heads, sizeof(hparams.n_heads));
        fin.read((char *)&hparams.n_layers, sizeof(hparams.n_layers));
        fin.read((char *)&hparams.n_vocab, sizeof(hparams.n_vocab));
        fin.read((char *)&hparams.ftype, sizeof(hparams.ftype));

        const int32_t qntvr = hparams.ftype / GGML_dadbed9_QNT_VERSION_FACTOR;

        printf("%s: d_model      = %d\n", __func__, hparams.d_model);
        printf("%s: max_seq_len  = %d\n", __func__, hparams.max_seq_len);
        printf("%s: n_heads      = %d\n", __func__, hparams.n_heads);
        printf("%s: n_layers     = %d\n", __func__, hparams.n_layers);
        printf("%s: n_vocab      = %d\n", __func__, hparams.n_vocab);
        printf("%s: ftype        = %d\n", __func__, hparams.ftype);
        printf("%s: qntvr        = %d\n", __func__, qntvr);

        hparams.ftype %= GGML_dadbed9_QNT_VERSION_FACTOR;
    }

    // load vocab
    replit_tokenizer_load(vocab, fin, model.hparams.n_vocab);

    // for the big tensors, we have the option to store the data in 16-bit
    // floats or quantized in order to save memory and also to speed up the
    // computation
    ggml_dadbed9_type wtype = ggml_dadbed9_ftype_to_ggml_dadbed9_type((ggml_dadbed9_ftype)(model.hparams.ftype));
    if (wtype == GGML_dadbed9_TYPE_COUNT) {
        fprintf(stderr, "%s: invalid model file '%s' (bad ftype value %d)\n", __func__, fname.c_str(),
                model.hparams.ftype);
        return false;
    }

    auto & ctx = model.ctx;

    size_t ctx_size = 0;

    {
        const auto & hparams = model.hparams;

        const int n_embd = hparams.d_model;
        const int n_layer = hparams.n_layers;
        const int n_ctx = hparams.max_seq_len;
        const int n_vocab = hparams.n_vocab;

        ctx_size += n_embd * n_vocab * ggml_dadbed9_type_sizef(wtype); // wte_weight
        ctx_size += n_embd * ggml_dadbed9_type_sizef(GGML_dadbed9_TYPE_F32);   // ln_f_weight

        ctx_size += n_layer * (n_embd * ggml_dadbed9_type_sizef(GGML_dadbed9_TYPE_F32));      // ln_1_weight
        ctx_size += n_layer * (3 * n_embd * n_embd * ggml_dadbed9_type_sizef(wtype)); // attn_Wqkv_weight
        ctx_size += n_layer * (n_embd * n_embd * ggml_dadbed9_type_sizef(wtype));     // attn_out_proj_weight
        ctx_size += n_layer * (n_embd * ggml_dadbed9_type_sizef(GGML_dadbed9_TYPE_F32));      // ln_2_weight
        ctx_size += n_layer * (4 * n_embd * n_embd * ggml_dadbed9_type_sizef(wtype)); // mlp_mlp_up_weight
        ctx_size += n_layer * (n_embd * n_embd * 4 * ggml_dadbed9_type_sizef(wtype)); // mlp_mlp_down_weight

        ctx_size += n_ctx * n_layer * n_embd * ggml_dadbed9_type_sizef(GGML_dadbed9_TYPE_F16); // memory_k
        ctx_size += n_ctx * n_layer * n_embd * ggml_dadbed9_type_sizef(GGML_dadbed9_TYPE_F16); // memory_v

        ctx_size += (1 + 6 * n_layer) * 512; // object overhead

        printf("%s: ggml ctx size = %6.2f MB\n", __func__, ctx_size / (1024.0 * 1024.0));
    }

    // create the ggml context
    {
        struct ggml_dadbed9_init_params params = {
            /*.mem_size   =*/ ctx_size,
            /*.mem_buffer =*/ NULL,
            /*.no_alloc   =*/ false,
        };

        model.ctx = ggml_dadbed9_init(params);
        if (!model.ctx) {
            fprintf(stderr, "%s: ggml_dadbed9_init() failed\n", __func__);
            return false;
        }
    }

    // prepare memory for the weights
    {
        const auto & hparams = model.hparams;

        const size_t n_embd = hparams.d_model;
        const size_t n_layer = hparams.n_layers;
        const size_t n_vocab = hparams.n_vocab;

        model.layers.resize(n_layer);

        model.wte_weight = ggml_dadbed9_new_tensor_2d(ctx, wtype, n_embd, n_vocab);
        model.norm_f_weight = ggml_dadbed9_new_tensor_1d(ctx, GGML_dadbed9_TYPE_F32, n_embd);

        // map by name
        model.tensors["transformer.wte.weight"] = model.wte_weight;
        model.tensors["transformer.norm_f.weight"] = model.norm_f_weight;

        for (int i = 0; i < (int)n_layer; ++i) {
            auto & layer = model.layers[i];

            layer.norm_1_weight = ggml_dadbed9_new_tensor_1d(ctx, GGML_dadbed9_TYPE_F32, n_embd);
            layer.c_attn_wqkv_weight = ggml_dadbed9_new_tensor_2d(ctx, wtype, n_embd, 3 * n_embd);
            layer.c_attn_out_proj_weight = ggml_dadbed9_new_tensor_2d(ctx, wtype, n_embd, n_embd);
            layer.norm_2_weight = ggml_dadbed9_new_tensor_1d(ctx, GGML_dadbed9_TYPE_F32, n_embd);
            layer.ffn_up_proj = ggml_dadbed9_new_tensor_2d(ctx, wtype, n_embd, 4 * n_embd);
            layer.ffn_down_proj = ggml_dadbed9_new_tensor_2d(ctx, wtype, 4 * n_embd, n_embd);

            // map by name
            model.tensors["transformer.blocks." + std::to_string(i) + ".norm_1.weight"] = layer.norm_1_weight;
            model.tensors["transformer.blocks." + std::to_string(i) + ".attn.Wqkv.weight"] = layer.c_attn_wqkv_weight;
            model.tensors["transformer.blocks." + std::to_string(i) + ".attn.out_proj.weight"] =
                layer.c_attn_out_proj_weight;
            model.tensors["transformer.blocks." + std::to_string(i) + ".norm_2.weight"] = layer.norm_2_weight;
            model.tensors["transformer.blocks." + std::to_string(i) + ".ffn.up_proj.weight"] = layer.ffn_up_proj;
            model.tensors["transformer.blocks." + std::to_string(i) + ".ffn.down_proj.weight"] = layer.ffn_down_proj;
        }
    }

    // key + value memory
    {
        const auto & hparams = model.hparams;

        const int n_embd = hparams.d_model;
        const int n_layer = hparams.n_layers;
        const int n_ctx = hparams.max_seq_len;

        const int64_t n_mem = n_layer * n_ctx;
        const int64_t n_elements = n_embd * n_mem;

        model.memory_k = ggml_dadbed9_new_tensor_1d(ctx, GGML_dadbed9_TYPE_F16, n_elements);
        model.memory_v = ggml_dadbed9_new_tensor_1d(ctx, GGML_dadbed9_TYPE_F16, n_elements);

        const size_t memory_size = ggml_dadbed9_nbytes(model.memory_k) + ggml_dadbed9_nbytes(model.memory_v);

        printf("%s: memory_size = %8.2f MB, n_mem = %lld\n", __func__, memory_size / 1024.0 / 1024.0, n_mem);
    }

    // load weights
    {
        int n_tensors = 0;
        size_t total_size = 0;

        printf("%s: ", __func__);

        while (true) {
            int32_t n_dims;
            int32_t length;
            int32_t ttype;

            fin.read(reinterpret_cast<char *>(&n_dims), sizeof(n_dims));
            fin.read(reinterpret_cast<char *>(&length), sizeof(length));
            fin.read(reinterpret_cast<char *>(&ttype), sizeof(ttype));

            if (fin.eof()) {
                break;
            }

            int32_t nelements = 1;
            int32_t ne[2] = {1, 1};
            for (int i = 0; i < n_dims; ++i) {
                fin.read(reinterpret_cast<char *>(&ne[i]), sizeof(ne[i]));
                nelements *= ne[i];
            }

            std::string name(length, 0);
            fin.read(&name[0], length);

            if (model.tensors.find(name.data()) == model.tensors.end()) {
                fprintf(stderr, "%s: unknown tensor '%s' in model file\n", __func__, name.data());
                return false;
            }

            auto tensor = model.tensors[name.data()];
            if (ggml_dadbed9_nelements(tensor) != nelements) {
                fprintf(stderr, "%s: tensor '%s' has wrong size in model file\n", __func__, name.data());
                return false;
            }

            if (tensor->ne[0] != ne[0] || tensor->ne[1] != ne[1]) {
                fprintf(stderr,
                        "%s: tensor '%s' has wrong shape in model file: got [%5d, "
                        "%5d], expected [%5d, %5d]\n",
                        __func__, name.data(), (int)tensor->ne[0], (int)tensor->ne[1], ne[0], ne[1]);
                return false;
            }

            // for debugging
            if (0) {
                printf("%24s - [%5d, %5d], type = %6s, %6.2f MB, %9zu bytes\n", name.data(), ne[0], ne[1],
                       ggml_dadbed9_type_name(ggml_dadbed9_type(ttype)), ggml_dadbed9_nbytes(tensor) / 1024.0 / 1024.0, ggml_dadbed9_nbytes(tensor));
            }

            const size_t bpe = ggml_dadbed9_type_size(ggml_dadbed9_type(ttype));

            if ((nelements * bpe) / ggml_dadbed9_blck_size(tensor->type) != ggml_dadbed9_nbytes(tensor)) {
                fprintf(stderr,
                        "%s: tensor '%s' has wrong size in model file: got %zu, "
                        "expected %zu\n",
                        __func__, name.data(), ggml_dadbed9_nbytes(tensor), nelements * bpe);
                return false;
            }

            fin.read(reinterpret_cast<char *>(tensor->data), ggml_dadbed9_nbytes(tensor));

            total_size += ggml_dadbed9_nbytes(tensor);
            if (++n_tensors % 8 == 0) {
                printf(".");
                fflush(stdout);
            }
        }

        printf(" done\n");

        printf("%s: model size = %8.2f MB / num tensors = %d\n", __func__, total_size / 1024.0 / 1024.0, n_tensors);
    }

    fin.close();

    return true;
}

// evaluate the transformer
//
//   - model:     the model
//   - n_threads: number of threads to use
//   - n_past:    the context size so far
//   - embd_inp:  the embeddings of the tokens in the context
//   - embd_w:    the predicted logits for the next token
//
bool replit_eval(const replit_model & model, const int n_threads, const int n_past,
                 const std::vector<gpt_vocab::id> & embd_inp, std::vector<float> & embd_w, bool logits_all,
                 size_t & mem_per_token) {
    const int N = embd_inp.size();

    const auto & hparams = model.hparams;

    const int n_embd = hparams.d_model;
    const int n_layer = hparams.n_layers;
    const int n_head = hparams.n_heads;
    const int n_vocab = hparams.n_vocab;
    const int n_ctx = hparams.max_seq_len;

    static size_t buf_size = 256u * 1024 * 1024;
    static void * buf = malloc(buf_size);

    if (mem_per_token > 0 && mem_per_token * N > buf_size) {
        const size_t buf_size_new = 1.1 * (mem_per_token * N); // add 10% to account for ggml object overhead
        // printf("\n%s: reallocating buffer from %zu to %zu bytes\n", __func__,
        // buf_size, buf_size_new);

        // reallocate
        buf_size = buf_size_new;
        buf = realloc(buf, buf_size);
        if (buf == nullptr) {
            fprintf(stderr, "%s: failed to allocate %zu bytes\n", __func__, buf_size);
            return false;
        }
    }

    struct ggml_dadbed9_init_params params = {
        /*.mem_size   =*/ buf_size,
        /*.mem_buffer =*/ buf,
        /*.no_alloc   =*/ false,
    };

    struct ggml_dadbed9_context * ctx0 = ggml_dadbed9_init(params);
    struct ggml_dadbed9_cgraph gf = {};

    struct ggml_dadbed9_tensor * embd = ggml_dadbed9_new_tensor_1d(ctx0, GGML_dadbed9_TYPE_I32, N);
    memcpy(embd->data, embd_inp.data(), N * ggml_dadbed9_element_size(embd));

    struct ggml_dadbed9_tensor * inpL = ggml_dadbed9_get_rows(ctx0, model.wte_weight, embd);

    for (int il = 0; il < n_layer; ++il) {

        struct ggml_dadbed9_tensor * cur;

        // a = self.ln_1(x)
        {
            cur = ggml_dadbed9_norm(ctx0, inpL);

            cur = ggml_dadbed9_mul(ctx0, ggml_dadbed9_repeat(ctx0, model.layers[il].norm_1_weight, cur), cur);
        }

        // self-attention
        //  b, _, past_key_value = self.attn(a, past_key_value=past_key_value,
        //  attn_bias=attn_bias, attention_mask=attention_mask,
        //  is_causal=is_causal)
        {
            // compute QKV
            cur = ggml_dadbed9_mul_mat(ctx0, model.layers[il].c_attn_wqkv_weight, cur);

            struct ggml_dadbed9_tensor * Qcur = ggml_dadbed9_view_2d(ctx0, cur, n_embd, N, cur->nb[1], 0 * sizeof(float) * n_embd);
            struct ggml_dadbed9_tensor * Kcur = ggml_dadbed9_view_2d(ctx0, cur, n_embd, N, cur->nb[1], 1 * sizeof(float) * n_embd);
            struct ggml_dadbed9_tensor * Vcur = ggml_dadbed9_view_2d(ctx0, cur, n_embd, N, cur->nb[1], 2 * sizeof(float) * n_embd);

            // store key and value to memory
            {
                struct ggml_dadbed9_tensor * k =
                    ggml_dadbed9_view_1d(ctx0, model.memory_k, N * n_embd,
                                 (ggml_dadbed9_element_size(model.memory_k) * n_embd) * (il * n_ctx + n_past));
                struct ggml_dadbed9_tensor * v =
                    ggml_dadbed9_view_1d(ctx0, model.memory_v, N * n_embd,
                                 (ggml_dadbed9_element_size(model.memory_v) * n_embd) * (il * n_ctx + n_past));

                ggml_dadbed9_build_forward_expand(&gf, ggml_dadbed9_cpy(ctx0, Kcur, k));
                ggml_dadbed9_build_forward_expand(&gf, ggml_dadbed9_cpy(ctx0, Vcur, v));
            }

            // Q = Qcur.contiguous().view(n_embd/n_head, n_head, N).permute(0,
            // 2, 1, 3) [64, N, 12]
            struct ggml_dadbed9_tensor * Q = ggml_dadbed9_permute(
                ctx0, ggml_dadbed9_cpy(ctx0, Qcur, ggml_dadbed9_new_tensor_3d(ctx0, GGML_dadbed9_TYPE_F32, n_embd / n_head, n_head, N)), 0, 2,
                1, 3);

            // K = Kmem.view(n_embd/n_head, n_head, n_past + N).permute(0, 2, 1,
            // 3) [64, n_past + N, 12]
            struct ggml_dadbed9_tensor * K =
                ggml_dadbed9_permute(ctx0,
                             ggml_dadbed9_reshape_3d(ctx0,
                                             ggml_dadbed9_view_1d(ctx0, model.memory_k, (n_past + N) * n_embd,
                                                          il * n_ctx * ggml_dadbed9_element_size(model.memory_k) * n_embd),
                                             n_embd / n_head, n_head, n_past + N),
                             0, 2, 1, 3);
            // K * Q
            struct ggml_dadbed9_tensor * KQ = ggml_dadbed9_mul_mat(ctx0, K, Q);

            // KQ_scaled = KQ / sqrt(n_embd/n_head)
            struct ggml_dadbed9_tensor * KQ_scaled =
                ggml_dadbed9_scale(ctx0, KQ, ggml_dadbed9_new_f32(ctx0, 1.0f / sqrt(float(n_embd) / n_head)));

            struct ggml_dadbed9_tensor * KQ_scaled_alibi = ggml_dadbed9_alibi(ctx0, KQ_scaled, n_past, n_head, 8.0f);

            // KQ_masked = mask_past(KQ_scaled)
            struct ggml_dadbed9_tensor * KQ_masked = ggml_dadbed9_diag_mask_inf(ctx0, KQ_scaled_alibi, n_past);

            // KQ = soft_max(KQ_masked)
            struct ggml_dadbed9_tensor * KQ_soft_max = ggml_dadbed9_soft_max(ctx0, KQ_masked);

            // V_trans = Vmem.view(n_embd/n_head, n_head, n_past + N).permute(1,
            // 2, 0, 3).contiguous() [n_past + N, 64, 12]
            struct ggml_dadbed9_tensor * V_trans = ggml_dadbed9_cpy(
                ctx0,
                ggml_dadbed9_permute(ctx0,
                             ggml_dadbed9_reshape_3d(ctx0,
                                             ggml_dadbed9_view_1d(ctx0, model.memory_v, (n_past + N) * n_embd,
                                                          il * n_ctx * ggml_dadbed9_element_size(model.memory_v) * n_embd),
                                             n_embd / n_head, n_head, n_past + N),
                             1, 2, 0, 3),
                ggml_dadbed9_new_tensor_3d(ctx0, model.memory_v->type, n_past + N, n_embd / n_head, n_head));

            // KQV = transpose(V) * KQ_soft_max
            struct ggml_dadbed9_tensor * KQV = ggml_dadbed9_mul_mat(ctx0, V_trans, KQ_soft_max);

            // KQV_merged = KQV.permute(0, 2, 1, 3)
            struct ggml_dadbed9_tensor * KQV_merged = ggml_dadbed9_permute(ctx0, KQV, 0, 2, 1, 3);

            // cur = KQV_merged.contiguous().view(n_embd, N)
            cur = ggml_dadbed9_cpy(ctx0, KQV_merged, ggml_dadbed9_new_tensor_2d(ctx0, GGML_dadbed9_TYPE_F32, n_embd, N));

            // projection
            { cur = ggml_dadbed9_mul_mat(ctx0, model.layers[il].c_attn_out_proj_weight, cur); }
        }

        inpL = ggml_dadbed9_add(ctx0, inpL, cur);

        // m = self.ln_2(x)
        {
            cur = ggml_dadbed9_norm(ctx0, inpL);

            cur = ggml_dadbed9_mul(ctx0, ggml_dadbed9_repeat(ctx0, model.layers[il].norm_2_weight, cur), cur);
        }

        // n = self.mlp(m)
        {

            cur = ggml_dadbed9_mul_mat(ctx0, model.layers[il].ffn_up_proj, cur);

            // GELU activation
            cur = ggml_dadbed9_gelu(ctx0, cur);

            // projection
            // cur = proj_w*cur + proj_b
            cur = ggml_dadbed9_mul_mat(ctx0, model.layers[il].ffn_down_proj, cur);
        }

        // x = x + n
        inpL = ggml_dadbed9_add(ctx0, inpL, cur);
    }

    // norm
    {
        inpL = ggml_dadbed9_norm(ctx0, inpL);
        // inpL = ln_f_g*inpL
        inpL = ggml_dadbed9_mul(ctx0, ggml_dadbed9_repeat(ctx0, model.norm_f_weight, inpL), inpL);
    }

    // output embedding weight tied to input embedding
    inpL = ggml_dadbed9_mul_mat(ctx0, model.wte_weight, inpL);

    // logits -> probs
    // inpL = ggml_dadbed9_soft_max(ctx0, inpL);

    // run the computation
    ggml_dadbed9_build_forward_expand(&gf, inpL);
    ggml_dadbed9_graph_compute_with_ctx(ctx0, &gf, n_threads);

    // std::cout << "Qcur" << std::endl;
    // print_tensor(Qcur);

    // if (n_past%100 == 0) {
    // ggml_dadbed9_graph_print(&gf);
    // ggml_dadbed9_graph_dump_dot(&gf, NULL, "mpt-model.dot");
    // }

    if (logits_all) {
        // return result for all tokens
        embd_w.resize(n_vocab * N);
        memcpy(embd_w.data(), (float *)ggml_dadbed9_get_data(inpL), sizeof(float) * n_vocab * N);
    } else {
        // return result for just the last token
        embd_w.resize(n_vocab);
        memcpy(embd_w.data(), (float *)ggml_dadbed9_get_data(inpL) + (n_vocab * (N - 1)), sizeof(float) * n_vocab);
    }

    if (mem_per_token == 0) {
        mem_per_token = ggml_dadbed9_used_mem(ctx0) / N;
    }
    // printf("used_mem = %zu\n", ggml_dadbed9_used_mem(ctx0));

    ggml_dadbed9_free(ctx0);

    return true;
}



struct replit_context * replit_init_from_file(const char * path_model, struct gpt_context_params   params) {
    ggml_dadbed9_time_init();

    replit_context * ctx = new replit_context;

    if (params.seed <= 0) {
        params.seed = time(NULL);
    }

    ctx->rng = std::mt19937(params.seed);
    ctx->logits_all = params.logits_all;

    ggml_dadbed9_type memory_type = params.f16_kv ? GGML_dadbed9_TYPE_F16 : GGML_dadbed9_TYPE_F32;
    
    
//    replit_model_load(const std::string & fname, replit_model & model, replit_tokenizer & vocab)
    if (!replit_model_load(path_model, ctx->model, ctx->vocab)) {
        fprintf(stderr, "%s: failed to load model\n", __func__);
        delete ctx;
        return nullptr;
    }


    // reserve memory for context buffers
    if (!params.vocab_only) {
        if (!kv_cache_init(ctx->model.hparams, ctx->model.kv_self, memory_type, ctx->model.hparams.n_ctx)) {
            fprintf(stderr, "%s: kv_cache_init() failed for self-attention cache\n", __func__);
            delete ctx;
            return nullptr;
        }

        {
            const size_t memory_size = ggml_dadbed9_nbytes(ctx->model.kv_self.k) + ggml_dadbed9_nbytes(ctx->model.kv_self.v);
            fprintf(stderr, "%s: kv self size  = %7.2f MiB\n", __func__, memory_size / 1024.0 / 1024.0);
        }

        const auto & hparams = ctx->model.hparams;

        // resized during inference
        if (params.logits_all) {
            ctx->logits.reserve(hparams.n_ctx*hparams.n_vocab);
        } else {
            ctx->logits.reserve(hparams.n_vocab);
        }

        if (params.embedding){
            ctx->embedding.resize(hparams.n_embd);
        }

//        ctx->buf_compute.resize(MEM_REQ_EVAL().at(ctx->model.type));
//
//        ctx->buf_scratch[0].resize(MEM_REQ_SCRATCH0().at(ctx->model.type));
//        ctx->buf_scratch[1].resize(MEM_REQ_SCRATCH1().at(ctx->model.type));
    }

    return ctx;
}


int replit_tokenize(
        struct replit_context * ctx,
                  const char * text,
                 gpt_token * tokens,
                         int   n_max_tokens,
                        bool   add_bos) {
//    replit_tokenizer_tokenize(replit_tokenizer & tokenizer, const std::string & text)
    auto res = replit_tokenizer_tokenize(ctx->vocab, text);
    
    if (n_max_tokens < (int) res.size()) {
        fprintf(stderr, "%s: too many tokens\n", __func__);
        return -((int) res.size());
    }

    for (size_t i = 0; i < res.size(); i++) {
        tokens[i] = res[i];
    }

    return res.size();
}

int replit_init_logits(struct replit_context * ctx,int   n_threads){
    size_t mem_per_token = 0;
//    replit_eval(const replit_model & model, const int n_threads, const int n_past,
//                     const std::vector<gpt_vocab::id> & embd_inp, std::vector<float> & embd_w, bool logits_all,
//                     size_t & mem_per_token)
    if (!replit_eval(ctx->model, n_threads, 0, { 0, 1, 2, 3 }, ctx->logits, false, mem_per_token)) {
        fprintf(stderr, "%s: failed to eval\n", __func__);
        return 1;
    }
    return  0;
}

int replit_eval(
        struct replit_context * ctx,
           const replit_token * tokens,
                         int   n_tokens,
                         int   n_past,
                  int   n_threads) {
    
//    std::vector<float> logits;
    std::vector<gpt_vocab::id> embd;
    for (int i=0;i<n_tokens;i++){
        embd.push_back(tokens[i]);
    }
    size_t mem_per_token = 0;
//    replit_eval(ctx->model, n_threads, 0, { 0, 1, 2, 3 }, ctx->logits, mem_per_token);
    //    if (!gptneox_eval_internal(*ctx, tokens, n_tokens, n_past, n_threads)) {
    if (!replit_eval(ctx->model, n_threads, n_past, embd, ctx->logits, false, mem_per_token)) {
        fprintf(stderr, "%s: failed to eval\n", __func__);
        return 1;
    }
    // get a more accurate load time, upon first eval
    if (!ctx->has_evaluated_once) {
        ctx->t_load_us = ggml_dadbed9_time_us() - ctx->t_start_us;
        ctx->has_evaluated_once = true;
    }
    return 0;
}

int32_t replit_sample(struct replit_context * ctx, int top_k, float top_p, float temp) {
    const int64_t t_start_sample_us = ggml_dadbed9_time_us();
//    gpt_sample_top_k_top_p(vocab.raw_vocab, logits.data() + (logits.size() - n_vocab), top_k, top_p,
//                                temp, rng);
    int n_logits = ctx->vocab.raw_vocab.id_to_token.size();
    gpt_vocab::id smpl = gpt_sample_top_k_top_p(n_logits, ctx->logits.data() + (ctx->logits.size() - ctx->vocab.raw_vocab.id_to_token.size()), top_k, top_p, temp, ctx->rng);
    if (ctx) {
        ctx->t_sample_us += ggml_dadbed9_time_us() - t_start_sample_us;
    }
    return  smpl;
}

int32_t replit_n_logits(struct replit_context * ctx){
    return ctx->vocab.raw_vocab.id_to_token.size();
}

int32_t replit_sample_repeat(struct replit_context * ctx,
                               const int32_t * last_n_tokens_data,
                               size_t last_n_tokens_data_size,
                               int top_k, float top_p, float temp,
                               int repeat_last_n,
                               float repeat_penalty) {
    const int64_t t_start_sample_us = ggml_dadbed9_time_us();
    int n_logits = ctx->vocab.raw_vocab.id_to_token.size();
    gpt_vocab::id smpl = gpt_sample_top_k_top_p_repeat(n_logits, ctx->logits.data() + (ctx->logits.size() - ctx->vocab.raw_vocab.id_to_token.size()),
                                                       last_n_tokens_data,last_n_tokens_data_size,
                                                       top_k, top_p, temp,
                                                       repeat_last_n,repeat_penalty,
                                                       ctx->rng);
    if (ctx) {
        ctx->t_sample_us += ggml_dadbed9_time_us() - t_start_sample_us;
    }
    return  smpl;
}


char* replit_token_to_str_res = new char[3];
const char * replit_token_to_str(struct replit_context * ctx, gpt_token token) {
//    std::string replit_tokenizer_detokenize(replit_tokenizer & tokenizer, const std::vector<std::size_t> & tokens)
//    std::string res = replit_tokenizer_detokenize(ctx->vocab, {static_cast<std::size_t>(token)});
//    fprintf(stderr, "T %s", res.c_str());
    
    std::string text = ctx->vocab.raw_vocab.id_to_token[token];
//    std::string text;
//    for (auto tok : {static_cast<std::size_t>(token)}) {
//        text += ctx->vocab.raw_vocab.id_to_token[tok];
//    }
    auto denormalized_text = replace_all(text, ws_symbol, " ");
    strcpy(replit_token_to_str_res, denormalized_text.c_str());
    return replit_token_to_str_res;
    
}

//
//int test_run(int argc, char ** argv) {
//    const int64_t t_main_start_us = ggml_dadbed9_time_us();
//
//    gpt_params params;
//    params.model = "";
//
//    if (gpt_params_parse(argc, argv, params) == false) {
//        return 1;
//    }
//
//    if (params.seed < 0) {
//        params.seed = time(NULL);
//    }
//
//    printf("%s: seed = %d\n", __func__, params.seed);
//
//    std::mt19937 rng(params.seed);
////    if (params.prompt.empty()) {
////        if (!is_stdin_terminal()) {
////            std::string line;
////            while (std::getline(std::cin, line)) {
////                params.prompt = params.prompt + "\n" + line;
////            }
////        } else {
////            params.prompt = gpt_random_prompt(rng);
////        }
////    }
//
//    int64_t t_load_us = 0;
//
//    replit_tokenizer vocab;
//    replit_model model;
//
//    // load the model
//    {
//        const int64_t t_start_us = ggml_dadbed9_time_us();
//
//        if (!replit_model_load(params.model, model, vocab)) {
//            fprintf(stderr, "%s: failed to load model from '%s'\n", __func__, params.model.c_str());
//            return 1;
//        }
//
//        t_load_us = ggml_dadbed9_time_us() - t_start_us;
//    }
//
//    int n_past = 0;
//
//    int64_t t_sample_us = 0;
//    int64_t t_predict_us = 0;
//
//    std::vector<float> logits;
//
//    // tokenize the prompt
//    std::vector<std::size_t> embd_inp = replit_tokenizer_tokenize(vocab, params.prompt);
//
//    printf("%s: number of tokens in prompt = %zu\n", __func__, embd_inp.size());
//
//    for (int i = 0; i < embd_inp.size(); i++) {
//        printf("%s: token[%d] = %6zu\n", __func__, i, embd_inp[i]);
//        // vocab.id_to_token.at(embd_inp[i]).c_str()
//    }
//    printf("\n");
//
//    params.n_predict = std::min(params.n_predict, model.hparams.max_seq_len - (int)embd_inp.size());
//
//    std::vector<gpt_vocab::id> embd;
//
//    // determine the required inference memory per token:
//    size_t mem_per_token = 0;
//    replit_eval(model, params.n_threads, 0, {0, 1, 2, 3}, logits, false, mem_per_token);
//
//    for (int i = embd.size(); i < embd_inp.size() + params.n_predict; i++) {
//        // predict
//        if (embd.size() > 0) {
//            const int64_t t_start_us = ggml_dadbed9_time_us();
//
//            if (!replit_eval(model, params.n_threads, n_past, embd, logits, false, mem_per_token)) {
//                printf("Failed to predict\n");
//                return 1;
//            }
//
//            t_predict_us += ggml_dadbed9_time_us() - t_start_us;
//        }
//
//        n_past += embd.size();
//        embd.clear();
//
//        if (i >= embd_inp.size()) {
//            // sample next token
//            const int top_k = params.top_k;
//            const float top_p = params.top_p;
//            const float temp = params.temp;
//
//            const int n_vocab = model.hparams.n_vocab;
//
//            gpt_vocab::id id = 0;
//
//            {
//                const int64_t t_start_sample_us = ggml_dadbed9_time_us();
//
//                id = gpt_sample_top_k_top_p(vocab.raw_vocab, logits.data() + (logits.size() - n_vocab), top_k, top_p,
//                                            temp, rng);
//
//                t_sample_us += ggml_dadbed9_time_us() - t_start_sample_us;
//            }
//
//            // add it to the context
//            embd.push_back(id);
//        } else {
//            // if here, it means we are still processing the input prompt
//            for (int k = i; k < embd_inp.size(); k++) {
//                embd.push_back(embd_inp[k]);
//                if (embd.size() > params.n_batch) {
//                    break;
//                }
//            }
//            i += embd.size() - 1;
//        }
//
//        // display text
//        for (auto id : embd) {
//            printf("%s", replit_tokenizer_detokenize(vocab, {static_cast<std::size_t>(id)}).c_str());
//        }
//        fflush(stdout);
//
//        // end of text token
//        if (embd.back() == 0) {
//            break;
//        }
//    }
//
//    // report timing
//    {
//        const int64_t t_main_end_us = ggml_dadbed9_time_us();
//
//        printf("\n\n");
//        printf("%s: mem per token = %8zu bytes\n", __func__, mem_per_token);
//        printf("%s:     load time = %8.2f ms\n", __func__, t_load_us / 1000.0f);
//        printf("%s:   sample time = %8.2f ms\n", __func__, t_sample_us / 1000.0f);
//        printf("%s:  predict time = %8.2f ms / %.2f ms per token\n", __func__, t_predict_us / 1000.0f,
//               t_predict_us / 1000.0f / n_past);
//        printf("%s:    total time = %8.2f ms\n", __func__, (t_main_end_us - t_main_start_us) / 1000.0f);
//    }
//
//    ggml_dadbed9_free(model.ctx);
//
//    return 0;
//}
//
