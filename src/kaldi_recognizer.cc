// Copyright 2019-2020 Alpha Cephei Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kaldi_recognizer.h"
#include "json.h"
#include "fstext/fstext-utils.h"
#include "lat/sausages.h"
#include "language_model.h"
#include "ivector/plda.h"

using namespace fst;
using namespace kaldi::nnet3;
typedef unordered_map<string, Vector<BaseFloat>*, StringHasher> HashType;

KaldiRecognizer::KaldiRecognizer(Model *model, float sample_frequency) : model_(model), spk_model_(0), sample_frequency_(sample_frequency) {

    model_->Ref();

    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    if (!model_->hclg_fst_) {
        if (model_->hcl_fst_ && model_->g_fst_) {
            decode_fst_ = LookaheadComposeFst(*model_->hcl_fst_, *model_->g_fst_, model_->disambig_);
        } else {
            KALDI_ERR << "Can't create decoding graph";
        }
    }

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);

    InitState();
    InitRescoring();
}

KaldiRecognizer::KaldiRecognizer(Model *model, float sample_frequency, char const *grammar) : model_(model), spk_model_(0), sample_frequency_(sample_frequency)
{
    model_->Ref();

    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    if (model_->hcl_fst_) {
        json::JSON obj;
        obj = json::JSON::Load(grammar);

        if (obj.length() <= 0) {
            KALDI_WARN << "Expecting array of strings, got: '" << grammar << "'";
        } else {
            KALDI_LOG << obj;

            LanguageModelOptions opts;

            opts.ngram_order = 2;
            opts.discount = 0.5;

            LanguageModelEstimator estimator(opts);
            for (int i = 0; i < obj.length(); i++) {
                bool ok;
                string line = obj[i].ToString(ok);
                if (!ok) {
                    KALDI_ERR << "Expecting array of strings, got: '" << obj << "'";
                }

                std::vector<int32> sentence;
                stringstream ss(line);
                string token;
                while (getline(ss, token, ' ')) {
                    int32 id = model_->word_syms_->Find(token);
                    if (id == kNoSymbol) {
                        KALDI_WARN << "Ignoring word missing in vocabulary: '" << token << "'";
                    } else {
                        sentence.push_back(id);
                    }
                }
                estimator.AddCounts(sentence);
            }
            g_fst_ = new StdVectorFst();
            estimator.Estimate(g_fst_);

            decode_fst_ = LookaheadComposeFst(*model_->hcl_fst_, *g_fst_, model_->disambig_);
        }
    } else {
        KALDI_WARN << "Runtime graphs are not supported by this model";
    }

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);

    InitState();
    InitRescoring();
}

KaldiRecognizer::KaldiRecognizer(Model *model, float sample_frequency, SpkModel *spk_model) : model_(model), spk_model_(spk_model), sample_frequency_(sample_frequency) {

    model_->Ref();
    spk_model->Ref();

    feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    if (!model_->hclg_fst_) {
        if (model_->hcl_fst_ && model_->g_fst_) {
            decode_fst_ = LookaheadComposeFst(*model_->hcl_fst_, *model_->g_fst_, model_->disambig_);
        } else {
            KALDI_ERR << "Can't create decoding graph";
        }
    }

    decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);

    spk_feature_ = new OnlineMfcc(spk_model_->spkvector_mfcc_opts);

    InitState();
    InitRescoring();
}

KaldiRecognizer::~KaldiRecognizer() {
    delete decoder_;
    delete feature_pipeline_;
    delete silence_weighting_;
    delete g_fst_;
    delete decode_fst_;
    delete spk_feature_;
    delete lm_fst_;

    delete info;
    delete lm_to_subtract_det_backoff;
    delete lm_to_subtract_det_scale;
    delete lm_to_add_orig;
    delete lm_to_add;

    model_->Unref();
    if (spk_model_)
         spk_model_->Unref();
}

void KaldiRecognizer::InitState()
{
    frame_offset_ = 0;
    samples_processed_ = 0;
    samples_round_start_ = 0;

    state_ = RECOGNIZER_INITIALIZED;
}

void KaldiRecognizer::InitRescoring()
{
    if (model_->rnnlm_lm_fst_) {
        float lm_scale = 0.5;
        int lm_order = 4;

        info = new kaldi::rnnlm::RnnlmComputeStateInfo(model_->rnnlm_compute_opts, model_->rnnlm, model_->word_embedding_mat);
        lm_to_subtract_det_backoff = new fst::BackoffDeterministicOnDemandFst<fst::StdArc>(*model_->rnnlm_lm_fst_);
        lm_to_subtract_det_scale = new fst::ScaleDeterministicOnDemandFst(-lm_scale, lm_to_subtract_det_backoff);
        lm_to_add_orig = new kaldi::rnnlm::KaldiRnnlmDeterministicFst(lm_order, *info);
        lm_to_add = new fst::ScaleDeterministicOnDemandFst(lm_scale, lm_to_add_orig);

    } else if (model_->std_lm_fst_) {
        fst::CacheOptions cache_opts(true, 50000);
        fst::ArcMapFstOptions mapfst_opts(cache_opts);
        fst::StdToLatticeMapper<kaldi::BaseFloat> mapper;
        lm_fst_ = new fst::ArcMapFst<fst::StdArc, kaldi::LatticeArc, fst::StdToLatticeMapper<kaldi::BaseFloat> >(*model_->std_lm_fst_, mapper, mapfst_opts);
    }
}

void KaldiRecognizer::CleanUp()
{
    delete silence_weighting_;
    silence_weighting_ = new kaldi::OnlineSilenceWeighting(*model_->trans_model_, model_->feature_info_.silence_weighting_config, 3);

    if (decoder_)
       frame_offset_ += decoder_->NumFramesDecoded();

    // Each 10 minutes we drop the pipeline to save frontend memory in continuous processing
    // here we drop few frames remaining in the feature pipeline but hope it will not
    // cause a huge accuracy drop since it happens not very frequently.

    // Also restart if we retrieved final result already

    if (decoder_ == nullptr || state_ == RECOGNIZER_FINALIZED || frame_offset_ > 20000) {
        samples_round_start_ += samples_processed_;
        samples_processed_ = 0;
        frame_offset_ = 0;

        delete decoder_;
        delete feature_pipeline_;

        feature_pipeline_ = new kaldi::OnlineNnet2FeaturePipeline (model_->feature_info_);
        decoder_ = new kaldi::SingleUtteranceNnet3Decoder(model_->nnet3_decoding_config_,
            *model_->trans_model_,
            *model_->decodable_info_,
            model_->hclg_fst_ ? *model_->hclg_fst_ : *decode_fst_,
            feature_pipeline_);

        if (spk_model_) {
            delete spk_feature_;
            spk_feature_ = new OnlineMfcc(spk_model_->spkvector_mfcc_opts);
        }
    } else {
        decoder_->InitDecoding(frame_offset_);
    }
}

void KaldiRecognizer::UpdateSilenceWeights()
{
    if (silence_weighting_->Active() && feature_pipeline_->NumFramesReady() > 0 &&
        feature_pipeline_->IvectorFeature() != nullptr) {
        vector<pair<int32, BaseFloat> > delta_weights;
        silence_weighting_->ComputeCurrentTraceback(decoder_->Decoder());
        silence_weighting_->GetDeltaWeights(feature_pipeline_->NumFramesReady(),
                                          frame_offset_ * 3,
                                          &delta_weights);
        feature_pipeline_->UpdateFrameWeights(delta_weights);
    }
}

void KaldiRecognizer::SetMaxAlternatives(int max_alternatives)
{
    max_alternatives_ = max_alternatives;
}

void KaldiRecognizer::SetWords(bool words)
{
    words_ = words;
}

void KaldiRecognizer::SetSpkModel(SpkModel *spk_model)
{
    if (state_ == RECOGNIZER_RUNNING) {
        KALDI_ERR << "Can't add speaker model to already running recognizer";
        return;
    }
    spk_model_ = spk_model;
    spk_model_->Ref();
    spk_feature_ = new OnlineMfcc(spk_model_->spkvector_mfcc_opts);
}

bool KaldiRecognizer::AcceptWaveform(const char *data, int len)
{
    Vector<BaseFloat> wave;
    wave.Resize(len / 2, kUndefined);
    for (int i = 0; i < len / 2; i++)
        wave(i) = *(((short *)data) + i);
    return AcceptWaveform(wave);
}

bool KaldiRecognizer::AcceptWaveform(const short *sdata, int len)
{
    Vector<BaseFloat> wave;
    wave.Resize(len, kUndefined);
    for (int i = 0; i < len; i++)
        wave(i) = sdata[i];
    return AcceptWaveform(wave);
}

bool KaldiRecognizer::AcceptWaveform(const float *fdata, int len)
{
    Vector<BaseFloat> wave;
    wave.Resize(len, kUndefined);
    for (int i = 0; i < len; i++)
        wave(i) = fdata[i];
    return AcceptWaveform(wave);
}

bool KaldiRecognizer::AcceptWaveform(Vector<BaseFloat> &wdata)
{
    // Cleanup if we finalized previous utterance or the whole feature pipeline
    if (!(state_ == RECOGNIZER_RUNNING || state_ == RECOGNIZER_INITIALIZED)) {
        CleanUp();
    }
    state_ = RECOGNIZER_RUNNING;

    int step = static_cast<int>(sample_frequency_ * 0.2);
    for (int i = 0; i < wdata.Dim(); i+= step) {
        SubVector<BaseFloat> r = wdata.Range(i, std::min(step, wdata.Dim() - i));
        feature_pipeline_->AcceptWaveform(sample_frequency_, r);
        UpdateSilenceWeights();
        decoder_->AdvanceDecoding();
    }
    samples_processed_ += wdata.Dim();

    if (spk_feature_) {
        spk_feature_->AcceptWaveform(sample_frequency_, wdata);
    }

    if (decoder_->EndpointDetected(model_->endpoint_config_)) {
        return true;
    }

    return false;
}

// Computes an xvector from a chunk of speech features.
static void RunNnetComputation(const MatrixBase<BaseFloat> &features,
    const nnet3::Nnet &nnet, nnet3::CachingOptimizingCompiler *compiler,
    Vector<BaseFloat> *xvector) 
{
    nnet3::ComputationRequest request;
    request.need_model_derivative = false;
    request.store_component_stats = false;
    request.inputs.push_back(
    nnet3::IoSpecification("input", 0, features.NumRows()));
    nnet3::IoSpecification output_spec;
    output_spec.name = "output";
    output_spec.has_deriv = false;
    output_spec.indexes.resize(1);
    request.outputs.resize(1);
    request.outputs[0].Swap(&output_spec);
    shared_ptr<const nnet3::NnetComputation> computation = compiler->Compile(request);
    nnet3::Nnet *nnet_to_update = nullptr;  // we're not doing any update.
    nnet3::NnetComputer computer(nnet3::NnetComputeOptions(), *computation,
                    nnet, nnet_to_update);
    CuMatrix<BaseFloat> input_feats_cu(features);
    computer.AcceptInput("input", &input_feats_cu);
    computer.Run();
    CuMatrix<BaseFloat> cu_output;
    computer.GetOutputDestructive("output", &cu_output);
    xvector->Resize(cu_output.NumCols());
    xvector->CopyFromVec(cu_output.Row(0));
}

#define MIN_SPK_FEATS 50

bool KaldiRecognizer::GetSpkVector(Vector<BaseFloat> &out_xvector, int *num_spk_frames)
{
    vector<int32> nonsilence_frames;
    if (silence_weighting_->Active() && feature_pipeline_->NumFramesReady() > 0) {
        silence_weighting_->ComputeCurrentTraceback(decoder_->Decoder(), true);
        silence_weighting_->GetNonsilenceFrames(feature_pipeline_->NumFramesReady(),
                                          frame_offset_ * 3,
                                          &nonsilence_frames);
    }

    int num_frames = spk_feature_->NumFramesReady() - frame_offset_ * 3;
    Matrix<BaseFloat> mfcc(num_frames, spk_feature_->Dim());

    // Not very efficient, would be nice to have faster search
    int num_nonsilence_frames = 0;
    Vector<BaseFloat> feat(spk_feature_->Dim());

    for (int i = 0; i < num_frames; ++i) {
       if (std::find(nonsilence_frames.begin(),
                     nonsilence_frames.end(), i / 3) == nonsilence_frames.end()) {
           continue;
       }

       spk_feature_->GetFrame(i + frame_offset_ * 3, &feat);
       mfcc.CopyRowFromVec(feat, num_nonsilence_frames);
       num_nonsilence_frames++;
    }

    *num_spk_frames = num_nonsilence_frames;

    // Don't extract vector if not enough data
    if (num_nonsilence_frames < MIN_SPK_FEATS) {
        return false;
    }

    mfcc.Resize(num_nonsilence_frames, spk_feature_->Dim(), kCopyData);

    SlidingWindowCmnOptions cmvn_opts;
    cmvn_opts.center = true;
    cmvn_opts.cmn_window = 300;
    Matrix<BaseFloat> features(mfcc.NumRows(), mfcc.NumCols(), kUndefined);
    SlidingWindowCmn(cmvn_opts, mfcc, &features);

    nnet3::NnetSimpleComputationOptions opts;
    nnet3::CachingOptimizingCompilerOptions compiler_config;
    nnet3::CachingOptimizingCompiler compiler(spk_model_->speaker_nnet, opts.optimize_config, compiler_config);

    Vector<BaseFloat> xvector;
    RunNnetComputation(features, spk_model_->speaker_nnet, &compiler, &xvector);

    out_xvector.Resize(spk_model_->transform.NumRows(), kSetZero);
    out_xvector.AddMatVec(1.0, spk_model_->transform, kNoTrans, xvector, 0.0);

    BaseFloat norm = out_xvector.Norm(2.0);
    BaseFloat ratio = norm / sqrt(out_xvector.Dim()); // how much larger it is
                                                  // than it would be, in
                                                  // expectation, if normally
    out_xvector.Scale(1.0 / ratio);

    xvector_result = xvector;
    PldaScoring();

    return true;
}


const char *KaldiRecognizer::MbrResult(CompactLattice &rlat)
{
    CompactLattice aligned_lat;
    if (model_->winfo_) {
        WordAlignLattice(rlat, *model_->trans_model_, *model_->winfo_, 0, &aligned_lat);
    } else {
        aligned_lat = rlat;
    }

    MinimumBayesRisk mbr(aligned_lat);
    const vector<BaseFloat> &conf = mbr.GetOneBestConfidences();
    const vector<int32> &words = mbr.GetOneBest();
    const vector<pair<BaseFloat, BaseFloat> > &times =
          mbr.GetOneBestTimes();

    int size = words.size();

    json::JSON obj;
    stringstream text;

    // Create JSON object
    for (int i = 0; i < size; i++) {
        json::JSON word;

        if (words_) {
            word["word"] = model_->word_syms_->Find(words[i]);
            word["start"] = samples_round_start_ / sample_frequency_ + (frame_offset_ + times[i].first) * 0.03;
            word["end"] = samples_round_start_ / sample_frequency_ + (frame_offset_ + times[i].second) * 0.03;
            word["conf"] = conf[i];
            obj["result"].append(word);
        }

        if (i) {
            text << " ";
        }
        text << model_->word_syms_->Find(words[i]);
    }
    obj["text"] = text.str();

    if (spk_model_) {
        Vector<BaseFloat> xvector;
        int num_spk_frames;
        if (GetSpkVector(xvector, &num_spk_frames)) {
            for (int i = 0; i < xvector.Dim(); i++) {
                obj["spk"].append(xvector(i));
            }
            obj["spk_frames"] = num_spk_frames;

            using pair_type = decltype(scores_)::value_type;
            auto pr = std::max_element
            (
                std::begin(scores_), std::end(scores_), [] (const pair_type & p1, const pair_type & p2) {
                    return p1.second < p2.second;
                }
            );
            KALDI_LOG << "speaker " << pr -> first << " score " << pr -> second;

            for (auto const &x : scores_) {
                json::JSON res;
                res["speaker"] = x.first;
                res["score"] = x.second;
                obj["scores"].append(res);
            }
            scores_.clear();
        }
    }

    return StoreReturn(obj.dump());
}

static bool CompactLatticeToWordAlignmentWeight(const CompactLattice &clat,
                                                std::vector<int32> *words,
                                                std::vector<int32> *begin_times,
                                                std::vector<int32> *lengths,
                                                CompactLattice::Weight *tot_weight_out)
{
  typedef CompactLattice::Arc Arc;
  typedef Arc::Label Label;
  typedef CompactLattice::StateId StateId;
  typedef CompactLattice::Weight Weight;
  using namespace fst;

  words->clear();
  begin_times->clear();
  lengths->clear();
  *tot_weight_out = Weight::Zero();

  StateId state = clat.Start();
  Weight tot_weight = Weight::One();

  int32 cur_time = 0;
  if (state == kNoStateId) {
    KALDI_WARN << "Empty lattice.";
    return false;
  }
  while (1) {
    Weight final = clat.Final(state);
    size_t num_arcs = clat.NumArcs(state);
    if (final != Weight::Zero()) {
      if (num_arcs != 0) {
        KALDI_WARN << "Lattice is not linear.";
        return false;
      }
      if (!final.String().empty()) {
        KALDI_WARN << "Lattice has alignments on final-weight: probably "
            "was not word-aligned (alignments will be approximate)";
      }
      tot_weight = Times(final, tot_weight);
      *tot_weight_out = tot_weight;
      return true;
    } else {
      if (num_arcs != 1) {
        KALDI_WARN << "Lattice is not linear: num-arcs = " << num_arcs;
        return false;
      }
      fst::ArcIterator<CompactLattice> aiter(clat, state);
      const Arc &arc = aiter.Value();
      Label word_id = arc.ilabel; // Note: ilabel==olabel, since acceptor.
      // Also note: word_id may be zero; we output it anyway.
      int32 length = arc.weight.String().size();
      words->push_back(word_id);
      begin_times->push_back(cur_time);
      lengths->push_back(length);
      tot_weight = Times(arc.weight, tot_weight);
      cur_time += length;
      state = arc.nextstate;
    }
  }
}


const char *KaldiRecognizer::NbestResult(CompactLattice &clat)
{
    Lattice lat;
    Lattice nbest_lat;
    std::vector<Lattice> nbest_lats;

    ConvertLattice (clat, &lat);
    fst::ShortestPath(lat, &nbest_lat, max_alternatives_);
    fst::ConvertNbestToVector(nbest_lat, &nbest_lats);

    json::JSON obj;
    std::stringstream ss;
    for (int k = 0; k < nbest_lats.size(); k++) {

      Lattice nlat = nbest_lats[k];
      RmEpsilon(&nlat);
      CompactLattice nclat;
      CompactLattice aligned_nclat;
      ConvertLattice(nlat, &nclat);

      if (model_->winfo_) {
          WordAlignLattice(nclat, *model_->trans_model_, *model_->winfo_, 0, &aligned_nclat);
      } else {
          aligned_nclat = nclat;
      }

      std::vector<int32> words;
      std::vector<int32> begin_times;
      std::vector<int32> lengths;
      CompactLattice::Weight weight;

      CompactLatticeToWordAlignmentWeight(aligned_nclat, &words, &begin_times, &lengths, &weight);
      float likelihood = -(weight.Weight().Value1() + weight.Weight().Value2());

      stringstream text;
      json::JSON entry;

      for (int i = 0; i < words.size(); i++) {
        json::JSON word;
        if (words[i] == 0)
            continue;
        if (words_) {
            word["word"] = model_->word_syms_->Find(words[i]);
            word["start"] = samples_round_start_ / sample_frequency_ + (frame_offset_ + begin_times[i]) * 0.03;
            word["end"] = samples_round_start_ / sample_frequency_ + (frame_offset_ + begin_times[i] + lengths[i]) * 0.03;
            entry["result"].append(word);
        }
        if (i)
          text << " ";
        text << model_->word_syms_->Find(words[i]);
      }

      entry["text"] = text.str();
      entry["confidence"]= likelihood;
      obj["alternatives"].append(entry);
    }

    return StoreReturn(obj.dump());
}

const char* KaldiRecognizer::GetResult()
{
    if (decoder_->NumFramesDecoded() == 0) {
        return StoreEmptyReturn();
    }

    kaldi::CompactLattice clat;
    kaldi::CompactLattice rlat;
    decoder_->GetLattice(true, &clat);

    if (model_->rnnlm_lm_fst_) {
        kaldi::ComposeLatticePrunedOptions compose_opts;
        compose_opts.lattice_compose_beam = 3.0;
        compose_opts.max_arcs = 3000;

        TopSortCompactLatticeIfNeeded(&clat);
        fst::ComposeDeterministicOnDemandFst<fst::StdArc> combined_lms(lm_to_subtract_det_scale, lm_to_add);
        CompactLattice composed_clat;
        ComposeCompactLatticePruned(compose_opts, clat,
                                    &combined_lms, &rlat);
        lm_to_add_orig->Clear();
    } else if (model_->std_lm_fst_) {
        Lattice lat1;

        ConvertLattice(clat, &lat1);
        fst::ScaleLattice(fst::GraphLatticeScale(-1.0), &lat1);
        fst::ArcSort(&lat1, fst::OLabelCompare<kaldi::LatticeArc>());
        kaldi::Lattice composed_lat;
        fst::Compose(lat1, *lm_fst_, &composed_lat);
        fst::Invert(&composed_lat);
        kaldi::CompactLattice determinized_lat;
        DeterminizeLattice(composed_lat, &determinized_lat);
        fst::ScaleLattice(fst::GraphLatticeScale(-1), &determinized_lat);
        fst::ArcSort(&determinized_lat, fst::OLabelCompare<kaldi::CompactLatticeArc>());

        kaldi::ConstArpaLmDeterministicFst const_arpa_fst(model_->const_arpa_);
        kaldi::CompactLattice composed_clat;
        kaldi::ComposeCompactLatticeDeterministic(determinized_lat, &const_arpa_fst, &composed_clat);
        kaldi::Lattice composed_lat1;
        ConvertLattice(composed_clat, &composed_lat1);
        fst::Invert(&composed_lat1);
        DeterminizeLattice(composed_lat1, &rlat);
    } else {
        rlat = clat;
    }

    fst::ScaleLattice(fst::GraphLatticeScale(0.9), &rlat); // Apply rescoring weight

    if (max_alternatives_ == 0) {
        return MbrResult(rlat);
    } else {
        return NbestResult(rlat);
    }

}


const char* KaldiRecognizer::PartialResult()
{
    if (state_ != RECOGNIZER_RUNNING) {
        return StoreEmptyReturn();
    }

    json::JSON res;

    if (decoder_->NumFramesDecoded() == 0) {
        res["partial"] = "";
        return StoreReturn(res.dump());
    }

    kaldi::Lattice lat;
    decoder_->GetBestPath(false, &lat);
    vector<kaldi::int32> alignment, words;
    LatticeWeight weight;
    GetLinearSymbolSequence(lat, &alignment, &words, &weight);

    ostringstream text;
    for (size_t i = 0; i < words.size(); i++) {
        if (i) {
            text << " ";
        }
        text << model_->word_syms_->Find(words[i]);
    }
    res["partial"] = text.str();

    return StoreReturn(res.dump());
}

const char* KaldiRecognizer::Result()
{
    if (state_ != RECOGNIZER_RUNNING) {
        return StoreEmptyReturn();
    }
    decoder_->FinalizeDecoding();
    state_ = RECOGNIZER_ENDPOINT;
    return GetResult();
}

const char* KaldiRecognizer::FinalResult()
{
    if (state_ != RECOGNIZER_RUNNING) {
        return StoreEmptyReturn();
    }

    feature_pipeline_->InputFinished();
    UpdateSilenceWeights();
    decoder_->AdvanceDecoding();
    decoder_->FinalizeDecoding();
    state_ = RECOGNIZER_FINALIZED;
    GetResult();

    // Free some memory while we are finalized, next
    // iteration will reinitialize them anyway
    delete decoder_;
    delete feature_pipeline_;
    delete silence_weighting_;
    delete spk_feature_;

    feature_pipeline_ = nullptr;
    silence_weighting_ = nullptr;
    decoder_ = nullptr;
    spk_feature_ = nullptr;

    return last_result_.c_str();
}

void KaldiRecognizer::Reset()
{
    if (state_ == RECOGNIZER_RUNNING) {
        decoder_->FinalizeDecoding();
    }
    StoreEmptyReturn();
    state_ = RECOGNIZER_ENDPOINT;
}

const char *KaldiRecognizer::StoreEmptyReturn()
{
    if (!max_alternatives_) {
        return StoreReturn("{\"text\": \"\"}");
    } else {
        return StoreReturn("{\"alternatives\" : [{\"text\": \"\", \"confidence\" : 1.0}] }");
    }
}

// Store result in recognizer and return as const string
const char *KaldiRecognizer::StoreReturn(const string &res)
{
    last_result_ = res;
    return last_result_.c_str();
}

void KaldiRecognizer::PldaScoring() {

    int64 num_train_ivectors = 0, num_train_errs = 0, num_test_ivectors = 0;
    double tot_test_renorm_scale = 0.0, tot_train_renorm_scale = 0.0;
    HashType test_ivectors;
    KALDI_LOG << "Reading test iVectors";
    std::string utt = "default";
    if (test_ivectors.count(utt) != 0) {
        KALDI_ERR << "Duplicate test iVector found for utterance " << utt;
    }

    xvector_result.AddVec(-1.0, spk_model_->mean);
    const Vector <BaseFloat> &vec(xvector_result);
    int32 transform_rows = spk_model_->transform.NumRows();
    int32 transform_cols = spk_model_->transform.NumCols();
    int32 vec_dim = vec.Dim();
    Vector <BaseFloat> vec_out(transform_rows);
    if (transform_cols == vec_dim) {
        vec_out.AddMatVec(1.0, spk_model_->transform, kNoTrans, vec, 0.0);
    } else {
        if (transform_cols != vec_dim + 1) {
            KALDI_ERR << "Dimension mismatch: input vector has dimension "
                      << vec.Dim() << " and transform has " << transform_cols
                      << " columns.";
        }
        vec_out.CopyColFromMat(spk_model_->transform, vec_dim);
        vec_out.AddMatVec(1.0, spk_model_->transform.Range(0, spk_model_->transform.NumRows(),
                                                           0, vec_dim), kNoTrans, vec, 1.0);
    }

    int32 num_examples = 1;// this value is always used for test (affects the
                           // length normalization in the TransformIvector
                           // function).
    Plda plda(spk_model_->plda);

    int32 plda_dim = plda.Dim();
    Vector <BaseFloat> *transformed_ivector = new Vector<BaseFloat>(plda_dim);
    tot_test_renorm_scale += plda.TransformIvector(spk_model_->plda_config, vec_out,
                                                   num_examples,
                                                   transformed_ivector);
    test_ivectors[utt] = transformed_ivector;
    bool binary = false;

    double sums = 0.0, sumsq = 0.0;
    typedef unordered_map<string, Vector < BaseFloat>*, StringHasher > HashType;
    std::map <std::string, int32> speakers = spk_model_->num_utts;
    for (auto const &x : speakers) {
        std::string key1 = x.first;
        if (spk_model_->train_ivectors.count(key1) == 0) {
            KALDI_WARN << "Key " << key1 << " not present in training iVectors.";
            continue;
        }
        if (test_ivectors.count(utt) == 0) {
            KALDI_WARN << "Key " << utt << " not present in test iVectors.";
            continue;
        }
        const Vector <BaseFloat> *train_ivector = spk_model_->train_ivectors[key1];
        const Vector <BaseFloat> *test_ivector = test_ivectors[utt];

        Vector<double> train_ivector_dbl(*train_ivector),
                test_ivector_dbl(*test_ivector);
        //  LOG: VECTORS
        std::cout << "TRAIN_VECTOR:\n[";
        double norm_test = 0, norm_train = 0;
        double norm_dot = 0;
        for (int32 i = 0; i < train_ivector_dbl.Dim(); ++i)
        {
            std::cout << train_ivector_dbl(i) << ' ';
            norm_train += train_ivector_dbl(i) * train_ivector_dbl(i);
        }
        std::cout << "]\nTEST_VECTOR:\n[";
        for (int32 i = 0; i < test_ivector_dbl.Dim(); ++i)
        {
            std::cout << test_ivector_dbl(i) << ' ';
            norm_test += test_ivector_dbl(i) * test_ivector_dbl(i);
            norm_dot += test_ivector_dbl(i) * train_ivector_dbl(i)
        }
        KALDI_LOG << "COS(" << key1 << ", test) = " << norm_dot / (norm_test * norm_train);

        int32 num_train_examples;
        num_train_examples = spk_model_->num_utts[key1];

        BaseFloat score = plda.LogLikelihoodRatio(train_ivector_dbl,
                                                  num_train_examples,
                                                  test_ivector_dbl);
        sums += score;
        sumsq += score * score;

        scores_.insert(std::pair<std::string, BaseFloat>(key1, score));
    }

//    for (HashType::iterator iter = spk_model_->train_ivectors.begin();
//         iter != spk_model_->train_ivectors.end(); ++iter)
//        delete iter->second;
    for (HashType::iterator iter = test_ivectors.begin();
         iter != test_ivectors.end(); ++iter)
        delete iter->second;
}
