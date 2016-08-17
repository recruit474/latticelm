/*
* Copyright 2010, Graham Neubig
* 
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
* 
*     http://www.apache.org/licenses/LICENSE-2.0
* 
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#ifndef LATTICELM_H
#define LATTICELM_H

#include "singlesample.h"
#include "pylm.h"
#include "lexfst.h"
#include "pylmfst.h"
#include "weighted-mapper.h"
#include "sampgen.h"
#include <unordered_map>
#include <fst/compose.h>
#include <fst/prune.h>
#include <fst/arcsort.h>

#define MAX_WORD_LEN 1e3

using namespace std;
using namespace pylm;
using namespace fst;

namespace latticelm {

class LatticeLM {

private:

    // type definitions
    typedef PhiMatcher< Matcher<Fst<StdArc> > > PM;

    // iteration parameters
    unsigned numBurnIn_; // the number of burn in (20)
    unsigned numAnnealSteps_; // the number of annealing steps (5)
    unsigned annealStepLength_; // the number of steps to anneal for (3)
    unsigned numSamples_; // the number of samples to take (100)
    unsigned sampleRate_; // the number of iterations between samples (1)
    unsigned trimRate_; // the number of iterations between trims (1)

    // training parameters
    double pruneThreshold_; // prune paths this far away (0, no pruning)
    double amScale_; // how much to scale the acoustic model (0.2)
    unsigned knownN_; // the n-gram size of the known word LM (3)
    unsigned unkN_; // the n-gram size of the unk LM (3)

    // input parameters
    const char* inputFileList_; // the list of files to be input
    vector< string > inputFiles_; // the files to be used as input

    static const unsigned INPUT_FST = 0;  // use FST input
    static const unsigned INPUT_TEXT = 1; // use text input
    unsigned inputType_;           // the type of input to use

    bool cacheInput_;
    vector< Fst<StdArc> * > inputFsts_; // the FSTs, if cached
    const char* symbolFile_; // a file containing the symbols

    // output parameters
    string prefix_; // the prefix of the output
    string separator_; // the character to use to separate the characters

    // training variables
    vector<unsigned> mySamples_; // which samples to use
    vector< vector<WordId> > histories_;
    unsigned unkSymbolSize_;
    double annealLevel_;

    // data structure
    LexFst<WordId, CharId> * lexFst_;
    PyLM<WordId> * knownLm_;
    PyLM<CharId> * unkLm_;
    vector<LMProb> unkBases_;

    // information variables
    double latticeLikelihood_; // the likelihood of the acoustic model
    double knownLikelihood_; // the likelihood of words generated by the LM
    double unkLikelihood_; // the likelihood of words generated by the unknown model


public:

    LatticeLM() : numBurnIn_(20), numAnnealSteps_(5), annealStepLength_(3),
        numSamples_(100), sampleRate_(1), trimRate_(1),
        pruneThreshold_(0), amScale_(0.2), knownN_(3), unkN_(3),
        inputFileList_(0), inputType_(INPUT_TEXT),
        cacheInput_(false), symbolFile_(0),
        prefix_(), separator_(), unkSymbolSize_(0), annealLevel_(0),
        lexFst_(0), knownLm_(0), unkLm_(0), unkBases_()
    {

    }

    ~LatticeLM() {
        if(lexFst_)  delete lexFst_;
        if(knownLm_) delete knownLm_;
        if(unkLm_)   delete unkLm_;
    }

    void dieOnHelp(const char* err) {
        cerr << "---latticelm v. 0.2 (9/21/2010)---" << endl
<< " A tool for learning a language model and a word dictionary" << endl
<< " from lattices (or text) using Pitman-Yor language models and" << endl
<< " Weighted Finite State Transducers" << endl
<< "  By Graham Neubig" << endl << endl
<< "Usage: latticelm -prefix out/ input.txt" << endl
<< "Options:" << endl
<< "  -burnin:       The number of iteration to execute as burn-in (20)" << endl
<< "  -annealsteps:  The number of annealing steps to perform (5)" << endl
<< "                 See Goldwater+ 2009 for details on annealing." << endl
<< "  -anneallength: The length of each annealing step in iterations (3)" << endl
<< "  -samps:        The number of samples to take (100)" << endl
<< "  -samprate:     The frequency (in iterations) at which to take samples (1)" << endl
<< "  -knownn:       The n-gram length of the language model (3)" << endl
<< "  -unkn:         The n-gram length of the spelling model (3)" << endl
<< "  -prune:        If this is activated, paths that are worse than the" << endl
<< "                 best path by at least this much will be pruned." << endl
<< "  -input:        The type of input (text/fst, default text)." << endl
<< "  -filelist:     A list of input files, one file per line." << endl
<< "                 For fst input, files must be in OpenFST binary "<<endl
<< "                 format, tropical semiring. Text files consist of one" << endl
<< "                 sentence per line." << endl
<< "  -symbolfile:   The symbol file for the WFSTs, not used for text input." << endl
<< "  -prefix:       The prefix under which to print all output." << endl
<< "  -separator:    The string to use to separate 'characters'." << endl
<< "  -cacheinput:   For WFST input, cache the WFSTs in memory (otherwise" << endl
<< "                 they will be loaded from disk every iteration)." << endl;
        if(err)
            cerr << endl << "Error: " << err << endl;
        exit(1);
    }

    CharId findId(const string & str, std::unordered_map<string,CharId> & idHash, vector<string> & idList) {
        std::unordered_map<string,CharId>::iterator it = idHash.find(str);
        if(it == idHash.end()) {
            idHash.insert(pair<string,CharId>(str,idHash.size()));
            idList.push_back("x"+str);
            return idHash.size()-1;
        }
        return it->second;
    }
    vector<string> loadText() {
        std::unordered_map<string,CharId> idHash;
        vector<string> idList;
        int state;
        // NOTE: <unk> and </unk> are included in the output vocab,
        //       but not the input. They must not occur in the input
        //       file.
        findId("<eps>",idHash,idList);
        findId("<phi>",idHash,idList);
        idList.push_back("x<unk>");
        idList.push_back("x</unk>");
        for(unsigned i = 0; i < inputFiles_.size(); i++) {
            ifstream in(inputFiles_[i].c_str());
            string line, str;
            while(getline(in,line)) {
                istringstream iss(line);
                StdVectorFst * fst = new StdVectorFst;
                fst->AddState();
                fst->SetStart(0);
                for(state = 0; iss >> str; state++) {
                    CharId lab = findId(str,idHash,idList);
                    fst->AddState();
                    fst->AddArc(state,StdArc(lab,lab,0,state+1));
                }
                fst->SetFinal(state,0);
                if(state == 0) {
                    cerr << "Empty line found in "<<inputFiles_[i]<<endl;
                    cerr << "Please ensure that each line in the training file contains at least one symbol."<<endl;
                    exit(1);
                }
                inputFsts_.push_back(fst);
            }
        }
        idList.push_back("w<s>");
        // idList.push_back("</s>");
        return idList;
    }

    void loadProperties(int argc, char** argv) {
        // read the arguments
        CharId argPos = 1;
        ostringstream err;
        for( ; argPos < argc && argv[argPos][0] == '-'; argPos++) {
            if(!strcmp(argv[argPos],"-burnin")) numBurnIn_ = atoi(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-annealsteps")) numAnnealSteps_ = atoi(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-anneallength")) annealStepLength_ = atoi(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-samps")) numSamples_ = atoi(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-samprate")) sampleRate_ = atoi(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-knownn")) knownN_ = atoi(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-unkn")) unkN_ = atoi(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-prune")) pruneThreshold_ = atof(argv[++argPos]);
            else if(!strcmp(argv[argPos],"-filelist")) inputFileList_ = argv[++argPos];
            else if(!strcmp(argv[argPos],"-input")) {
                ++argPos;
                if(!strcmp("fst",argv[argPos]))  inputType_ = INPUT_FST;
                else if(!strcmp("text",argv[argPos])) inputType_ = INPUT_TEXT;
                else {
                    err << "Bad input type '"<<argv[argPos]<<"'";
                    dieOnHelp(err.str().c_str());
                }
            }
            else if(!strcmp(argv[argPos],"-symbolfile")) symbolFile_ = argv[++argPos];
            else if(!strcmp(argv[argPos],"-prefix"))     prefix_ = argv[++argPos];
            else if(!strcmp(argv[argPos],"-separator"))  separator_ = argv[++argPos];
            else if(!strcmp(argv[argPos],"-cacheinput")) cacheInput_ = true;
            else {
                err << "Illegal option: " << argv[argPos];
                dieOnHelp(err.str().c_str());
            }
        }
        if(inputType_ == INPUT_TEXT) cacheInput_ = true;
 
        // load the input files, either from the list or not
        if(inputFileList_) {
            ifstream files(inputFileList_);
            if(!files) {
                err << "Couldn't find the file list: "<<inputFileList_ <<endl;
                dieOnHelp(err.str().c_str());
            }
            string buff;
            while(getline(files,buff)) {
                inputFiles_.push_back(buff);
                ifstream checkFile(buff.c_str());
                if(!checkFile) {
                    err << "Couldn't find input file: '" << buff << "'" << endl;
                    dieOnHelp(err.str().c_str());
                }
                checkFile.close();
            }
            files.close();
        } else {
            for( ; argPos < argc; argPos++) {
                inputFiles_.push_back(string(argv[argPos]));
                ifstream checkFile(argv[argPos]);
                if(!checkFile) {
                    err << "Couldn't find input file: " << argv[argPos] <<endl;
                    dieOnHelp(err.str().c_str());
                }
                checkFile.close();
            }
        }
        lexFst_ = new LexFst<WordId,CharId>();
        lexFst_->setSeparator(separator_);
        // sanity check for the FST input
        if(inputType_ == INPUT_FST) {
            inputFsts_.resize(inputFiles_.size(),0);
            if(!symbolFile_)
                dieOnHelp("No symbol file was set");
            lexFst_->load(symbolFile_);
        }
        // load the text input
        else { 
            lexFst_->setPermSymbols(loadText());
            lexFst_->initializeArcs();
        }
        histories_.resize(inputFsts_.size());

        // load the symbols for the lexicon FST
        unkSymbolSize_ = lexFst_->getNumChars();
        unkBases_.resize(MAX_WORD_LEN,1.0/unkSymbolSize_);
        cerr << "Loaded " << unkSymbolSize_ << " symbols";
        if(symbolFile_) cerr << " from " << symbolFile_;
        cerr << endl;

        // load the LMs
        knownLm_ = new PyLM<WordId>(knownN_);
        unkLm_ = new PyLM<CharId>(unkN_);

        // perform sanity check
        if(inputFiles_.size() == 0)
            dieOnHelp("No input files specified");
        else if(prefix_.length() == 0)
            dieOnHelp("No output prefix was specified");

    }

    // train the model on all the data
    void train() {
        
        // initialize mySamples to 0,1,2,3..n-1
        mySamples_ = vector<unsigned>(inputFsts_.size());
        for(unsigned i = 0; i < mySamples_.size(); i++)
            mySamples_[i] = i;

        // iterate
        for(unsigned iter = 0; iter <= numSamples_; iter++) {
            
            // reset the information variables
            unkLikelihood_ = 0; knownLikelihood_ = 0; latticeLikelihood_ = 0;
            
            // set annealLevel appropriately
            annealLevel_ = (int)(iter+annealStepLength_-1)/annealStepLength_;
            if(annealLevel_ != 0)
                annealLevel_ = 1.0/max(1.0,numAnnealSteps_-annealLevel_);
            
            // iterate
            iterateSamples(annealLevel_);

            // sample the model parameters and print status
            sampleParameters();
            printIterationStatus(iter);
        
            // trim down the size if necessary
            if(iter%trimRate_ == 0)
                trimModels();

            // print a sample if necessary
            if(iter >= numBurnIn_ && (iter-numBurnIn_)%sampleRate_==0) {
                cerr << " Printing sample for iteration "<<iter<<endl;
                printSample(iter);
            }

        }

    }

    // trim the models, removing unneeded vocabulary
    void trimModels() {
        // trim the language model
        vector<WordId> trimmedIds = knownLm_->trim(true);
        unkLm_->trim(false);
        // trim the lexicon
        const vector< vector<CharId> > & knownWords = lexFst_->getWords();
        LexFst<WordId,CharId> * nextLex = new LexFst<WordId,CharId>;
        nextLex->setSeparator(separator_);
        nextLex->setPermSymbols(lexFst_->getPermSymbols());
        nextLex->initializeArcs();
        for(unsigned i = 0; i < knownWords.size(); i++) {
            if(trimmedIds[i] != -1)
                nextLex->addWord(knownWords[i]);
        }
        // const vector< string > & knownSymbols = lexFst_->getSymbols();
        // const vector< string > & newSymbols = nextLex->getSymbols();
        // re-map the history
        for(unsigned i = 0; i < histories_.size(); i++)
            for(unsigned j = 0; j < histories_[i].size(); j++)
                histories_[i][j] = trimmedIds[histories_[i][j]];
        delete lexFst_;
        lexFst_ = nextLex;
    }

    // print the status of the current iteration
    void printIterationStatus(unsigned int iter, ostream & out = cerr) {
        
        out << "Finished iteration " << iter << " (Anneal="<<annealLevel_<<"), LM="<< (knownLikelihood_+unkLikelihood_) 
            << " (w=" << knownLikelihood_ << ", u="<<unkLikelihood_<<"), Lattice=" << latticeLikelihood_ << endl
             << " Vocabulary: w=" << knownLm_->getVocabSize() <<", u="<<unkLm_->getVocabSize() << endl
             << " LM size: w=" << knownLm_->size() <<", u="<<unkLm_->size() << endl;
        for(int i = 0; i < knownLm_->getN(); i++)
            out << " WLM " << (i+1) << "-gram, s="<<knownLm_->getStrength(i)<<", d="<<knownLm_->getDiscount(i)<<endl;
        for(int i = 0; i < unkLm_->getN(); i++)
            out << " CLM " << (i+1) << "-gram, s="<<unkLm_->getStrength(i)<<", d="<<unkLm_->getDiscount(i)<<endl;
    }
    
    // sample the model parameters
    void sampleParameters() {
        knownLm_->sampleParameters();
        unkLm_->sampleParameters();
    }

    // print a single sample to the appropriate file
    void printSample(int iter = -1) {
        const vector<string> & symbols = lexFst_->getSymbols();
        // const vector< vector<CharId> > & words = lexFst_->getWords();
        writeLm(unkLm_,&symbols[2],&unkBases_[0],prefix_+"ulm",iter);
        const vector< LMProb > wordBases = calculateWordBases();
        writeLm(knownLm_,&symbols[2+unkSymbolSize_],&wordBases[0],prefix_+"wlm",iter);
        writeSamples(&symbols[2+unkSymbolSize_],prefix_+"samp",iter);
        // TODO print step fst
        // TODO cumulate language models
        // TODO print cumulated language model
        // TODO print cumulated fst
        writeSymbols(prefix_+"sym",iter);
    }

    void iterateSamples(double annealLevel) {
        unsigned step = mySamples_.size()/100 + 1;
        cerr << "Running on "<<inputFsts_.size()<<" sequences (\".\"="<<step<<" sequences, \"!\"="<<step*10<<" sequences)"<<endl;
        time_t start = time(NULL);
        for(unsigned i = 0; i < mySamples_.size(); i++) {
            singleSample(mySamples_[i], annealLevel);
            if(i%step == step-1)
                cerr << (i/step%10 == 9 ? '!' : '.');
        }
        cerr << ' ' << (time(NULL)-start) << " seconds" << endl;
    }

    void singleSample(unsigned sentId, double annealLevel = 1) {
        if(histories_[sentId].size())
            removeSample(sentId);

        // build
        Fst<StdArc> * inputFst = createInputFst(sentId);
        ComposeFst<StdArc> ilFst(*inputFst, *lexFst_);

        PylmFst<WordId,CharId> pylmFst(*knownLm_, *unkLm_, unkSymbolSize_);
        ComposeFstOptions<StdArc, PM> copts(CacheOptions(),
                              new PM(ilFst, MATCH_NONE),
                              new PM(pylmFst, MATCH_INPUT,1));
        ComposeFst<StdArc> ilpFst(ilFst, pylmFst, copts);

        // prune
        VectorFst<StdArc> prunedFst;
        if(pruneThreshold_ != 0)
            Prune<StdArc>(ilpFst,&prunedFst,pruneThreshold_);
        else
            prunedFst = VectorFst<StdArc>(ilpFst);
        // check to make sure that pruning worked correctly
        if(prunedFst.NumStates() <= 1) {
            VectorFst<StdArc>(*inputFst).Write("inputFst.fst");
            VectorFst<StdArc>(ilFst).Write("ilFst.fst");
            VectorFst<StdArc>(ilpFst).Write("ilpFst.fst");
            VectorFst<StdArc>(pylmFst).Write("pylmFst.fst");
            THROW_ERROR("Pruned FST has one or fewer states\n");
        }
        // sample
        VectorFst<StdArc> sampledFst;
        SampGen(prunedFst, sampledFst, 1, annealLevel);
        // save and add
        histories_[sentId] = lexFst_->parseSample(sampledFst);
        // for(unsigned i = 0; i < histories_[sentId].size(); i++)
        //     cerr << histories_[sentId][i] << " ";
        //     cerr << endl;
        addSample(sentId);
        if(!cacheInput_)
            delete inputFst;
        // calculate the likelihood
        StdArc::StateId sid = sampledFst.Start();
        while(true) {
            ArcIterator< Fst<StdArc> > ai(sampledFst,sid);
            if(ai.Done()) break;
            latticeLikelihood_ += ai.Value().weight.Value();
            sid = ai.Value().nextstate;
        }
    }

    // remove a sample from the LMs
    void removeSample(unsigned sentId) { 
        knownLm_->removeCustomers(histories_[sentId]);
        const vector<int> & remPositions = knownLm_->getBasePositions();
        const vector< vector<CharId> > & knownWords = lexFst_->getWords();
        for(unsigned j = 0; j < remPositions.size(); j++)
            unkLm_->removeCustomers(knownWords[histories_[sentId][remPositions[j]]]);
    }

    // add the sample to the LMs
    void addSample(unsigned sentId) {
        const vector<WordId> & words = histories_[sentId];
        const vector< vector<CharId> > & knownWords = lexFst_->getWords();
        // get the word base probabilities
        vector<LMProb> knownBases(words.size(),0);
        for(unsigned j = 0; j < words.size(); j++) 
            knownBases[j] = exp(unkLm_->calcSentence(knownWords[words[j]], unkBases_, false));
        // sample the LM and save the probability
        knownLikelihood_ -= knownLm_->calcSentence(words, knownBases, true);
        const vector<int> & addPositions = knownLm_->getBasePositions();
        for(unsigned j = 0; j < addPositions.size(); j++) 
            unkLikelihood_ -= unkLm_->calcSentence(knownWords[words[addPositions[j]]], unkBases_, true);
    }

    // create (or load) an FST representing the data
    Fst<StdArc> * createInputFst(unsigned sentId) {
        // cerr << "createInputFst("<<sentId<<") "<<cacheInput_<<", "<<inputFsts_.size()<<", "<<(int)inputFsts_[sentId]<<endl;
        if(cacheInput_ && inputFsts_.size() > sentId && inputFsts_[sentId])
            return inputFsts_[sentId];
        Fst<StdArc>* ret = NULL;
        WeightedMapper mapper(amScale_);
        Fst<StdArc> * nextFst = VectorFst<StdArc>::Read(inputFiles_[sentId]);
        ret = new VectorFst<StdArc>;
        Map(*nextFst, (VectorFst<StdArc>*)ret, mapper);
        ArcSort((VectorFst<StdArc>*)ret,OLabelCompare<StdArc>());
        delete nextFst;
        if(cacheInput_) {
            if(inputFsts_.size() <= sentId) inputFsts_.resize(sentId+1,0);
            inputFsts_[sentId] = ret;
        }
        return ret;
    }

    // get the word base probabilities
    vector<LMProb> calculateWordBases() {
        const vector< vector<CharId> > & knownWords = lexFst_->getWords();
        vector<LMProb> bases(knownWords.size(),0);
        for(unsigned j = 0; j < knownWords.size(); j++) 
            bases[j] = exp(unkLm_->calcSentence(knownWords[j], unkBases_, false));
        return bases;
    }

    // write out the symbol file
    void writeSymbols(string fileName, int iter = -1) {
        if(!fileName.length())
            fileName = prefix_+"sym";
        if(iter >= 0) {
            ostringstream oss; oss << fileName << '.' << iter; 
            fileName = oss.str();
        }
        cerr << "  Writing symbols to "<<fileName<<endl;
        ofstream symOut(fileName.c_str());
        const vector<string> & words = lexFst_->getSymbols();
        for(unsigned i = 0; i < words.size(); i++)
            symOut << words[i] << "\t" << i << endl;
        symOut.close();
    }

    // write out the LM file
    template <class T>
    void writeLm(const PyLM<T> * lm, const string* symbols, const LMProb* bases, string fileName, int iter = -1) {
        if(!fileName.length())
            fileName = prefix_+"lm";
        if(iter >= 0) {
            ostringstream oss; oss << fileName << '.' << iter; 
            fileName = oss.str();
        }
        cerr << "  Writing LM to "<<fileName<<endl;
        ofstream lmOut(fileName.c_str());
        lm->print(symbols,bases,lmOut);
        lmOut.close();
    }

    // write out the samples
    void writeSamples(const string* symbols, string fileName, int iter = -1) {
        if(!fileName.length())
            fileName = prefix_+"samp";
        if(iter >= 0) {
            ostringstream oss; oss << fileName << '.' << iter; 
            fileName = oss.str();
        }
        cerr << "  Writing samples to "<<fileName<<endl;
        ofstream sampOut(fileName.c_str());
        for(unsigned i = 0; i < histories_.size(); i++) {
            for(unsigned j = 0; j < histories_[i].size(); j++) {
                if(j) sampOut << " ";
                sampOut << symbols[histories_[i][j]].substr(1);
            }
            sampOut << endl;
        }
        sampOut.close();
    }

///////////////////////
// utility functions //
///////////////////////
private:

};

}

#endif
