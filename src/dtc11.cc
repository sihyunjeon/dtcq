#include <include/FIFO.h>
#include <interface/Circuit.h>
#include <include/Component.h>
#include <include/Ports.h>
#include <ctime>
#include <deque>
#include <algorithm>
#include <iostream>
#include <boost/filesystem.hpp>
#include <fstream>
#include <stdint.h>
#include <memory>
#include <random>
#include <limits>
#include <assert.h>
#include <bitset>
#include <stdexcept>
#include <interface/EventBoundaryFinder.h>
using namespace std;
using namespace boost::filesystem;

typedef FIFO<uint64_t> FIFO64;
typedef FIFO<uint16_t> FIFO16;

bool DEBUG=false;
const bool RANDOM_L1=true;
const bool TRIGGER_RULE=true;
enum RATE_TYPE { NODELAY=0, HALF=1, FULL=2 };
const int OUTPUT_RATE=HALF;

// read data from binary file then split streams into different chip managers
class DataPlayerFromFile final : public Component
{
public:
    std::vector<OutputPort<bool>> out_read;
    std::vector<OutputPort<uint64_t>> out_data;

    DataPlayerFromFile(vector<string> file_name_list, int _nchips, vector<float> elink_chip_ratio) : Component(), out_read(_nchips), out_data(_nchips), nevents(_nchips, 0) {
        assert(_nchips == file_name_list.size());
        assert(_nchips == elink_chip_ratio.size());
        nchips = _nchips;
        for (int ichip=0; ichip<nchips; ichip++) {
            add_output( &(out_read[ichip]) );
            add_output( &(out_data[ichip]) );
            assert( elink_chip_ratio[ichip]>0 );
            ticks_per_word.push_back( int(ticks_per_word_per_elink/elink_chip_ratio[ichip]) );
            std::ifstream* new_stream = new std::ifstream(file_name_list[ichip].c_str(), std::ios::binary);
            if (!bool(*new_stream)) throw std::invalid_argument((string("Unable to open file ")+file_name_list[ichip]).c_str());
            input_streams.push_back( new_stream );
        }
        // edit bunch_not_empty according to LHC filling scheme
        bool* position_in_orbit = bunch_not_empty;
        for (int i=0;i<3;i++){
            for (int j=0; j<2; j++) {
                for (int k=0; k<3; k++) {
                    std::fill(position_in_orbit, position_in_orbit+72, true);
                    position_in_orbit+=72;
                    position_in_orbit+=8;
                }
                position_in_orbit+=30;
            }
            for (int k=0; k<4; k++) {
                std::fill(position_in_orbit, position_in_orbit+72, true);
                position_in_orbit+=72;
                position_in_orbit+=8;
            }
            position_in_orbit+=31;
        }
        for (int j=0; j<3; j++) {
            for (int k=0; k<3; k++) {
                std::fill(position_in_orbit, position_in_orbit+72, true);
                position_in_orbit+=72;
                position_in_orbit+=8;
            }
            position_in_orbit+=30;
        }
        position_in_orbit+=81;
        assert( position_in_orbit - bunch_not_empty == bunches_per_orbit );
    };

    void tick() override {
        if (RANDOM_L1) {
            // Check trigger every 25ns (10 clock ticks) at bunch crossings
            // Implemented trigger rule: no more than 8 triggers 130 bunch crossings
            if (nticks%10==0) {
                assert(nbunch<bunches_per_orbit);
                for (int i=0; i<time_since_recent_L1As.size(); i++) time_since_recent_L1As[i]++;
                if (time_since_recent_L1As.size()>0 && time_since_recent_L1As.front()>trigger_rule_bunch_period) time_since_recent_L1As.pop_front();
                int new_rand = rand();
                if (time_since_recent_L1As.size()<trigger_rule_max_L1As && bunch_not_empty[nbunch] && rand()%int(min_ticks_per_event/10)==0) {
                    triggered_events ++;
                    time_since_recent_L1As.push_back(0);
                }
                nbunch ++;
                if (nbunch == bunches_per_orbit) nbunch = 0;
            }
        }
        for (int ichip=0; ichip<nchips; ichip++) {
            // Condition to read a new word: 1. File not at EOF; 2. matches ticks_per_word to be consistent with e-link speed 3. Does not exceed trigger rate
            bool trigger_condition = false;
            if (RANDOM_L1)  trigger_condition = (triggered_events > nevents[ichip]);
            else trigger_condition = ( (min_ticks_per_event+nticks)/(1+nevents[ichip]) >= min_ticks_per_event );
            if ( (!input_streams[ichip]->eof()) && (nticks%ticks_per_word[ichip]==0) && trigger_condition) {
                value = 0;
                input_streams[ichip]->read( reinterpret_cast<char*>(&value), sizeof(value) ) ;
                if(value & (((uint64_t)1)<<63)) nevents[ichip]++;
                bitset<64> b_value(value);
                //std::cout<<"Event player chip "<<ichip<<" data = "<<b_value<<std::endl;
                //std::cout<<"Event player chip "<<ichip<<" input file get single character = "<<input_streams[ichip]->get()<<std::endl;
                out_read[ichip].set_value(true);
                out_data[ichip].set_value(value);
            }
            else {
                out_read[ichip].set_value(false);
                out_data[ichip].set_value(0);
            }
        }
        nticks ++;
    }
private:
    unsigned long long nticks = 0;
    int triggered_events = 0;
    int nchips;
    int nbunch = 0; // cyclic counting bunch from 0-3563;
    std::vector<std::ifstream*> input_streams;
    std::vector<int> nevents;
    uint64_t value;
    static const int ticks_per_word_per_elink = 20; // assuming all chip has rate of 1.28Gbps, 400M * 64 / 1.28G = 20
    std::vector<int> ticks_per_word; //ticks_per_word_per_elink divided by e-link-to-chip ratio
    static const int min_ticks_per_event =  533; // 400M / 750k = 533.3
    static const int bunches_per_orbit = 3564;
    bool bunch_not_empty[bunches_per_orbit] = {0}; //modified in initializer
    static const int trigger_rule_max_L1As = 8;
    static const int trigger_rule_bunch_period = 130; // No more than 8 L1As within 130 bunch crossings;
    std::deque<int> time_since_recent_L1As;
};

// only read data from the next event after all data from this event is processed
// processed = nothong done, for now
class EventBuilder final : public Component
{
public:
    std::vector<InputPort<bool>> in_data_valid ;
    std::vector<InputPort<uint64_t>> in_data   ;
    std::vector<InputPort<bool>> in_control_valid ;
    std::vector<InputPort<uint16_t>> in_control    ;
    std::vector<OutputPort<bool>> out_read_data;
    std::vector<OutputPort<bool>> out_read_control ;
    EventBuilder(int _nchips) : Component(), in_data_valid(_nchips), in_data(_nchips), in_control_valid(_nchips), in_control(_nchips), out_read_data(_nchips), out_read_control(_nchips),
    words_to_read(_nchips, 0), 
    control_full_event(_nchips, false), 
    read_control_last_time(_nchips, false), 
    read_data_last_time(_nchips, false), 
    buffer_counter(_nchips, 0),
    control_new_event_header(_nchips, false) {
        nchips = _nchips;
        for (int ichip=0; ichip<nchips; ichip++) {
            add_output( &(out_read_data[ichip]) );
            add_output( &(out_read_control[ichip]) );
        }
    };

    void tick() override {
        clock_ticks_counter ++;
        if (remaining_time_to_send_last_event > 0) remaining_time_to_send_last_event--;
        for (int ichip=0; ichip<nchips; ichip++) {
            if (in_data_valid[ichip].get_value() == true) {
                buffer_counter[ichip] += 1;
                words_to_read[ichip] -= 1;
                assert(words_to_read[ichip] >= 0);
            }
            if (in_control_valid[ichip].get_value() == true){
                //std::bitset<16> x(in_control[ichip].get_value());
                //std::cout<<"chip "<<ichip<<" control value = "<<x<<std::endl;
                assert(!control_full_event[ichip]);
                if ( (in_control[ichip].get_value() & (((uint16_t)1<<15))) && (buffer_counter[ichip]>0)) {
                    control_full_event[ichip] = true;
                    control_new_event_header[ichip] = true;
                }
                else {
                    words_to_read[ichip] += 1;
                }
            }
            bool read_data_this_time = (words_to_read[ichip]>1) || (words_to_read[ichip]==1 && !read_data_last_time[ichip]);
            out_read_data[ichip].set_value( read_data_this_time );
            read_data_last_time[ichip] = read_data_this_time;
            out_read_control[ichip].set_value( (!control_full_event[ichip]) && (!read_control_last_time[ichip]) );
            read_control_last_time[ichip]=( (!control_full_event[ichip]) && (!read_control_last_time[ichip]) ); // avoid consecutive control read, otherwise might read more word than needed due to signal delay.
        }
        //std::cout<<"EBD is full event status = ";
        //for (auto i : control_full_event) std::cout<<i;
        //std::cout<<std::endl;
        if (std::all_of(control_full_event.begin(), control_full_event.end(), [](bool v){return v;} ) && std::all_of(words_to_read.begin(), words_to_read.end(), [](int i){return i==0;} ) && (remaining_time_to_send_last_event == 0)) { // if all chips have full data for the event, and the last event has been sent out
            if (WORD_PER_CLOCK_TICK_TO_SEND_EVENT>0) {
                remaining_time_to_send_last_event = int(std::accumulate(buffer_counter.begin(), buffer_counter.end(), 0)/WORD_PER_CLOCK_TICK_TO_SEND_EVENT);
            }
            int maximum_number_of_words = *max_element(buffer_counter.begin(), buffer_counter.end());
            std::cout<<"New event processed after "<<clock_ticks_counter<<" clock ticks! with maximum number of words per chip = "<<maximum_number_of_words;
            std::cout<<" Will take "<<remaining_time_to_send_last_event<<" clock ticks to send it out."<<std::endl;
            clock_ticks_counter = 0;
            for (int ichip=0; ichip<nchips; ichip++) {
                control_full_event[ichip] = false;
                buffer_counter[ichip] = 0;
                if (control_new_event_header[ichip]) {
                    control_new_event_header[ichip] = false;
                    words_to_read[ichip] ++;
                }
            }
        }
    }
private:
    int nchips;
    std::vector<int> words_to_read;
    std::vector<int> buffer_counter; // instead of an actual buffer
    std::vector<bool> control_full_event;
    std::vector<bool> control_new_event_header;
    std::vector<bool> read_control_last_time;
    std::vector<bool> read_data_last_time;
    int clock_ticks_counter = 0;
    bool processing_new_event = true;
    const int WORD_PER_CLOCK_TICK_TO_SEND_EVENT = 8*OUTPUT_RATE; // equals to number of output links with 25GB/s speed. By design this can be up to 16.
    int remaining_time_to_send_last_event = 0;
};


int main() {
    // get a list of input files related to dtc11
    std::vector<std::string> dtc11_binary_fn_list;
    path input_dir("./input");
    for (auto iter_fn=directory_iterator(input_dir); iter_fn != directory_iterator(); iter_fn++) {
        if ( is_directory(iter_fn->path()) ) continue;
        string filename = iter_fn->path().string();
        if ( filename.find("dtc11") == std::string::npos ) continue;
        dtc11_binary_fn_list.push_back(filename);
    }
    int nchips = dtc11_binary_fn_list.size();
    std::cout<< "Number of chips mapped to DTC 11 = " << nchips <<endl;
    std::vector<float> elink_chip_ratio(nchips, 3); // All chips connected to DTC 11 has 3 e-links
    auto circuit = std::make_shared<Circuit>();
    auto player  = std::make_shared<DataPlayerFromFile>(dtc11_binary_fn_list, nchips, elink_chip_ratio); // there are 60 modules in dtc11
    auto evt_builder  = std::make_shared<EventBuilder>(nchips); 
    circuit->add_component(player);
    circuit->add_component(evt_builder);

    std::vector<std::shared_ptr<FIFO64>>              fifos_input;
    std::vector<std::shared_ptr<FIFO64>>              fifos_output_data;
    std::vector<std::shared_ptr<FIFO16>>               fifos_output_control;
    std::vector<std::shared_ptr<EventBoundaryFinder>>  ebfs;
    for (int ichip=0; ichip<nchips; ichip++){
        fifos_input.push_back(std::make_shared<FIFO64>());
        fifos_output_data.push_back(std::make_shared<FIFO64>());
        fifos_output_control.push_back(std::make_shared<FIFO16>());
        ebfs.push_back(std::make_shared<EventBoundaryFinder>());
        circuit->add_component(fifos_input[ichip]);
        circuit->add_component(fifos_output_data[ichip]);
        circuit->add_component(fifos_output_control[ichip]);
        circuit->add_component(ebfs[ichip] );
        player->out_data[ichip].connect( &(fifos_input[ichip]->in_data) );
        player->out_read[ichip].connect( &(fifos_input[ichip]->in_push_enable) );
        //Input FIFO <-> Boundary finder
        fifos_input[ichip]->out_data.connect( &(ebfs[ichip]->in_fifo_i1_data) );
        fifos_input[ichip]->out_data_valid.connect( &(ebfs[ichip]->in_fifo_i1_data_valid) );
        ebfs[ichip]->out_fifo_i1_pop.connect( &(fifos_input[ichip]->in_pop_enable) );
        //Boundary finder <-> output FIFO
        ebfs[ichip]->out_fifo_o1_data.connect( &(fifos_output_data[ichip]->in_data) );
        ebfs[ichip]->out_fifo_o1_read.connect( &(fifos_output_data[ichip]->in_push_enable) );
        ebfs[ichip]->out_fifo_o2_data.connect( &(fifos_output_control[ichip]->in_data) );
        ebfs[ichip]->out_fifo_o2_read.connect( &(fifos_output_control[ichip]->in_push_enable) );
        // Output FIFO <-> Event Builder
        fifos_output_data[ichip]->out_data.connect( &(evt_builder->in_data[ichip]) );
        fifos_output_data[ichip]->out_data_valid.connect( &(evt_builder->in_data_valid[ichip]) );
        fifos_output_control[ichip]->out_data.connect( &(evt_builder->in_control[ichip]) );
        fifos_output_control[ichip]->out_data_valid.connect( &(evt_builder->in_control_valid[ichip]) );
        evt_builder->out_read_data[ichip].connect( &(fifos_output_data[ichip]->in_pop_enable) );
        evt_builder->out_read_control[ichip].connect( &(fifos_output_control[ichip]->in_pop_enable) );
    }
    int inactive_time = 0;
    int max_inactive_time = 100000;
    int i_tick = 0;
    std::ofstream outputsize_input_fifo("output/input_fifo_sizes.txt");
    std::ofstream outputsize_output_fifo_data("output/output_fifo_data_sizes.txt");
    std::cout<<"auto-ticking..."<<std::endl;
    while (inactive_time<max_inactive_time )
    {
        //////////////////// DEBUG BLOCK ///////////////////////////////
        if (DEBUG) {                                                ////
            std::cout<<"itick="<<i_tick<<std::endl;
            // availability of Data player?
            std::cout<<"player->out_read:"<<std::endl;
            for (int ichip=0; ichip<nchips; ichip++) {
                if (ichip>0) std::cout<<",";
                std::cout<<to_string(player->out_read[ichip].get_value());
            }
            std::cout<<std::endl;
            // availability of Input FIFOs
            std::cout<<"fifos_input->out_data_valid:"<<std::endl;
            for (int ichip=0; ichip<nchips; ichip++) {
                if (ichip>0) std::cout<<",";
                std::cout<<to_string(fifos_input[ichip]->out_data_valid.get_value());
            }
            std::cout<<std::endl;
            // availability of Event Boudary Finder
            std::cout<<"fifos_input->out_data_valid:"<<std::endl;
            for (int ichip=0; ichip<nchips; ichip++) {
                if (ichip>0) std::cout<<",";
                std::cout<<to_string(ebfs[ichip]->out_fifo_o1_read.get_value());
            }
            std::cout<<std::endl;
            std::cout<<"fifos_input->out_control_valid:"<<std::endl;
            for (int ichip=0; ichip<nchips; ichip++) {
                if (ichip>0) std::cout<<",";
                std::cout<<to_string(ebfs[ichip]->out_fifo_o2_read.get_value());
            }
            std::cout<<std::endl;
            // availability of Output Control FIFO
            std::cout<<"fifos_output_control->out_data_valid:"<<std::endl;
            for (int ichip=0; ichip<nchips; ichip++) {
                if (ichip>0) std::cout<<",";
                std::cout<<to_string(fifos_output_control[ichip]->out_data_valid.get_value());
            }
            std::cout<<std::endl;
            // availability of Output Data FIFO
            std::cout<<"fifos_output_data->out_data_valid:"<<std::endl;
            for (int ichip=0; ichip<nchips; ichip++) {
                if (ichip>0) std::cout<<",";
                std::cout<<to_string(fifos_output_data[ichip]->out_data_valid.get_value());
            }
            std::cout<<std::endl;
            // occupancy of Output Data FIFO
            std::cout<<"fifos_output_data->get_buffer_size:"<<std::endl;
            for (int ichip=0; ichip<nchips; ichip++) {
                if (ichip>0) std::cout<<",";
                std::cout<<to_string(fifos_output_data[ichip]->d_get_buffer_size());
            }
            std::cout<<std::endl;
            char key='x';
            while ((key!='n') && (key!='q')) {
                std::cout<<"press \"n\" to continue next tick, \"q\" to skip debugging..."<<std::endl;
                std::cin>>key;
            }
            if (key == 'q') DEBUG=false;
        }                                                           ////
        //////////////////// DEBUG BLOCK ///////////////////////////////


        i_tick++;
        //std::cout<<"tick="<<i_tick<<std::endl;
        circuit->tick();
        bool activity=false;
        for (auto port:evt_builder->in_data_valid) activity = activity | port.get_value();
        if(!activity) inactive_time++;
        else inactive_time=0;
        for (int ichip=0; ichip<nchips; ichip++) {
            if (ichip>0) {outputsize_input_fifo<<","; outputsize_output_fifo_data<<",";}
            outputsize_input_fifo<<to_string(fifos_input[ichip]->d_get_buffer_size());
            outputsize_output_fifo_data<<to_string(fifos_output_data[ichip]->d_get_buffer_size());
        };
        outputsize_input_fifo<<endl;
        outputsize_output_fifo_data<<endl;
    }
    cout<<"total ticks="<<i_tick - max_inactive_time <<endl;
}