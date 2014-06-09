#include "common.h"
#include "globalVars.h"
#include "configFile.h"
#include "fileElem.h"
#include "kbGraph.h"
#include "disambGraph.h"
#include "wdict.h"
#include "ukbServer.h"
#include <string>
#include <iostream>
#include <fstream>

// Program options

#include <boost/program_options.hpp>

// timer

#include <boost/timer.hpp>

// bfs

#include <boost/graph/breadth_first_search.hpp>
#include <boost/pending/indirect_cmp.hpp>

#if BOOST_VERSION > 104400
  #include <boost/range/irange.hpp>
#else
  #include <boost/pending/integer_range.hpp>
#endif

using namespace std;
using namespace boost;
using namespace ukb;

const char *kb_default_binfile = "kb_wnet.bin";

static bool opt_normalize_ranks = true;

static bool output_control_line = false;
static bool output_variants_ppv = false;
static float trunc_ppv = 0.0;
static bool opt_nozero = false;
static string ppv_prefix;
static string cmdline("!! -v ");

// - sort all concepts according to their ppv weight, then scan the
// resulting sequence of concetps with a sliding window of length 100,
// and truncate the sequence when the difference in scores between the
// first and last concepts in the window drops below 5% of the
// highest-scoring concept for this word


struct CWSort {

  CWSort(const vector<float> & _v) : v(_v) {}
  int operator () (const int & i, const int & j) {
	// Descending order
	return v[i] > v[j];
  }
  const vector<float> & v;
};

// Fill with zero's all values below threshold

void truncate_ppv_with_zeros(vector<float> & ppv, float thres) {

  size_t n = ppv.size();
  if (n < 100) return;

  vector<int> idx(n);

  for(size_t i=0; i < n; ++i)
	idx[i] = i;
  sort(idx.begin(), idx.end(), CWSort(ppv));

  size_t cut_i = 99;
  float cut_th = ppv[idx[0]]*thres;
  for(; cut_i < n; ++cut_i) {
	if ((ppv[idx[cut_i-99]] - ppv[idx[cut_i]]) < cut_th) break;
  }

  // truncate ppv
  for(; cut_i < n; ++cut_i) {
	ppv[idx[cut_i]] = 0.0;
  }

  // Normalize result

  if(opt_normalize_ranks)
	normalize_pvector(ppv);

}

// Fill with zero's all values unless top K

void top_k(vector<float> & ppv, size_t k) {

  size_t n = ppv.size();
  if (k >= n) return;

  vector<int> idx(n);

  for(size_t i=0; i < n; ++i)
	idx[i] = i;
  sort(idx.begin(), idx.end(), CWSort(ppv));

  for(size_t i = k; i < n; ++i) {
	ppv[idx[i]] = 0.0;
  }
  if(opt_normalize_ranks)
	normalize_pvector(ppv);
}

static void post_process_ranks(vector<float> & theranks) {

  if (trunc_ppv > 0.0f) {
	if (trunc_ppv < 1.0f)
	  truncate_ppv_with_zeros(theranks, trunc_ppv);
	else {
	  // For top k calculation
	  //
	  //  - fill with zeros all values except top k
	  //  - set opt_nozero = 1 so only top k are printed
	  //
	  // * could be a problem if top k had zeros in it, as they
	  //   will not be properly printed.
	  top_k(theranks, lexical_cast<size_t>(trunc_ppv));
	  opt_nozero = true;
	}
  }
}

// write a rank vector to an ostream

static void write_ppv_stream(vector<float> & ranks, ostream & os) {

  Kb & kb = Kb::instance();

  post_process_ranks(ranks);

  if (output_control_line)
	os << cmdline << "\n";
  for(size_t i = 0, m = ranks.size(); i < m; ++i) {
	string sname = kb.get_vertex_name(i);
	if (opt_nozero && ranks[i] == 0.0) continue;
	os << sname << "\t" << ranks[i];
	if (output_variants_ppv) {
	  os << "\t" << WDict::instance().variant(sname);
	}
	os << "\n";
  }
}

static void write_ppv_stream(const vector<float> & outranks, ostream & os) {
  vector<float> newranks(outranks);
  write_ppv_stream(newranks, os);
}

// Get a filename for an output ppv

static boost::shared_ptr<ofstream> output_ppv_fname(const string & filename,
													File_elem & fout) {
  fout.fname = filename;

  boost::shared_ptr<ofstream> fo(new ofstream(fout.get_fname().c_str(), ofstream::out));
  if (!fo) {
	throw std::runtime_error(std::string("[E] Can not create ") + fout.get_fname());
  }
  return fo;
}

static void maybe_postproc_ranks(vector<float> & ranks) {
  if (glVars::csentence::disamb_minus_static) {
	const vector<float> & static_ranks = Kb::instance().static_prank();
	for(size_t s_i = 0, s_m = static_ranks.size();
		s_i != s_m; ++s_i) {
	  ranks[s_i] -= static_ranks[s_i];
	}
  }
}

// Compute ppv given a CSentence

bool compute_cs_ppv(CSentence & cs, vector<float> & ranks) {
  if (!calculate_kb_ppr(cs,ranks)) return false;
  maybe_postproc_ranks(ranks);
  return true;
}

// Get input from is, compute ppv and create output files under out_dir

void compute_sentence_vectors(istream & is, string & out_dir) {

  File_elem fout("lala", out_dir, ".ppv");

  CSentence cs;

  // Read sentences and compute rank vectors
  size_t l_n  = 0;
  while (cs.read_aw(is, l_n)) {
	// Initialize rank vector
	vector<float> ranks;
	if (!compute_cs_ppv(cs, ranks)) {
	  cerr << "Error when calculating ranks for csentence " << cs.id() << endl;
	  continue;
	}

	boost::shared_ptr<ofstream> fo(output_ppv_fname(ppv_prefix + cs.id(), fout));
	write_ppv_stream(ranks, *fo);
	cs = CSentence();
  }
}

// Compute static PPV and write to standard output

void compute_static_ppv() {


  // Calculate static (static) pageRank over KB
  const vector<float> & ranks = Kb::instance().static_prank();

  write_ppv_stream(ranks, cout);
}

///////////////////////////////////////////////
// Server/clien functions

// Sends PPV vector through socket
static void output_ppv_stream_socket(vector<float> & ranks,
									 sSession & session) {

  Kb & kb = Kb::instance();

  post_process_ranks(ranks);
  if (output_control_line)
	session.send(cmdline);
  for(size_t i = 0, m = ranks.size(); i < m; ++i) {
	string sname = kb.get_vertex_name(i);
	if (opt_nozero && ranks[i] == 0.0) continue;
	ostringstream oss;
	oss << sname << "\t" << ranks[i];
	if (output_variants_ppv) {
	  oss << "\t" << WDict::instance().variant(sname);
	}
	oss << "\n";
	session.send(oss.str());
  }
}

// Return FALSE means kill server

bool handle_server_read(sSession & session) {
  string ctx_id;
  string ctx;
  try {
	session.receive(ctx);
	if (ctx == "stop") return false;
	// TODO Check command is ppv
	while(1) {
	  if (!session.receive(ctx_id)) break;
	  if (!session.receive(ctx)) break;
	  CSentence cs(ctx_id, ctx);
	  vector<float> ranks;
	  if (!compute_cs_ppv(cs, ranks)) {
		// throw "Error when calculating ranks for csentence " << cs.id() << endl;
		// throw std::runtime_error(std::string("[E] when calculating ranks for csentence ") + cs.id() + ":" + this->error_str());
		continue;
	  }
	  output_ppv_stream_socket(ranks, session);
	  session.send("--END--PPV");
	}
  } catch (std::exception& e)	{
	// send error and close the session.
	// Note: the server is still alive for new connections.
	session.send(e.what());
  }
  return true;
}

// Send contexts to daemon, get output ppv ranks and write to proper files

bool client(istream & is, unsigned int port, const string & out_dir) {
  // connect to ukb port and send data to it.
  sClient client("localhost", port);
  string server_cmd;
  string go("ppv");
  if (client.error()) {
	std::cerr << "Error when connecting: " << client.error_str() << std::endl;
	return false;
  }
  string id, ctx, out;
  size_t l_n = 0;
  File_elem fout("lala", out_dir, ".ppv");
  try {
	client.send(go);
	// TODO Receive ack server is ppv
	while(read_line_noblank(is, id, l_n)) {
	  if(!read_line_noblank(is, ctx, l_n)) return false;
	  client.send(id);
	  client.send(ctx);
	  boost::shared_ptr<ofstream> fo(output_ppv_fname(ppv_prefix + id, fout));
	  while(1) {
		client.receive(out);
		if (out == "--END--PPV") break;
		*fo << out;
	  }
	}
  } catch (std::exception& e)	{
	std::cerr << e.what() << std::endl;
	return false;
  }
  return true;
}


bool client_stop_server(unsigned int port) {
  // connect to ukb port and tell it to stop
  sClient client("localhost", port);
  string stop("stop");
  if (client.error()) {
	std::cerr << "Error when connecting: " << client.error_str() << std::endl;
	return false;
  }
  try {
	client.send(stop);
  } catch (std::exception& e)	{
	std::cerr << e.what() << std::endl;
	return false;
  }
  return true;
}

void load_kb_dict() {}

int main(int argc, char *argv[]) {

  srand(3);

  bool opt_daemon = false;
  bool opt_static = false;
  bool opt_client = false;
  bool opt_shutdown = false;

  string kb_binfile;
  string alternative_dict_fname;

  cmdline += glVars::ukb_version;
  for (int i=0; i < argc; ++i) {
    cmdline += " ";
    cmdline += argv[i];
  }

  string out_dir(".");
  string fullname_in;
  ifstream input_ifs;

  unsigned int port = 10000;
  size_t iterations = 0;
  float thresh = 0.0;
  bool check_convergence = false;

  const char desc_header[] = "ukb_ppv: get personalized PageRank vector if a KB\n"
	"Usage examples:\n"
    "ukb_ppv -K kb.bin -D dict.txt -O outdir input.txt\n"
    "  Creates one file per sentence (.ppv extension) with the vector of the PPV vector given the input sentence"
    "Options";

  using namespace boost::program_options;

  options_description po_desc("General options:");

  po_desc.add_options()
    ("help,h", "This help page.")
    ("version", "Show version.")
    ("kb_binfile,K", value<string>(), "Binary file of KB (see compile_kb).")
    ("out_dir,O", value<string>(), "Directory for leaving output PPV files. Default is current directory.")
    ("static,S", "Compute static PageRank ppv. Only -K option is needed. Output to STDOUT.")
    ("dict_file,D", value<string>(), "Word to synset map file.")
    ("verbose,v", "Be verbose.")
    ;

  options_description po_desc_input("Input options");
  po_desc_input.add_options()
	("nopos", "Don't filter words by Part of Speech.")
	("minput", "Do not die when dealing with malformed input.")
	("ctx_noweight", "Do not use weights of input words (defaut is use context weights).")
    ;

  options_description po_desc_prank("pageRank general options");
  po_desc_prank.add_options()
    ("prank_weight,w", "Use weights in pageRank calculation. Serialized graph edges must have some weight.")
    ("prank_iter", value<size_t>(), "Number of iterations in pageRank. Default is 30.")
    ("prank_threshold", value<float>(), "Threshold for pageRank convergence. Default is 0.0001.")
    ("prank_damping", value<float>(), "Set damping factor in PageRank equation. Default is 0.85.")
    ;

  options_description po_desc_dict("Dictionary options");
  po_desc_dict.add_options()
	("altdict", value<string>(), "Provide an alternative dictionary overriding the values of default dictionary.")
    ("dict_weight", "Use weights when linking words to concepts (dict file has to have weights).")
    ("dict_weight_smooth", value<float>(), "Smoothing factor to be added to every weight in dictionary concepts. Default is 1.")
    ("dict_strict", "Be strict when reading the dictionary and stop when any error is found.")
    ;

  options_description po_desc_output("Output options");
  po_desc_output.add_options()
    ("nostatic", "Substract static ppv to final ranks.")
    ("trunc_ppv", value<float>(), "Truncate PPV threshold. If arg > 1, return top arg nodes.")
    ("trunc_topK", value<size_t>(), "Return top arg nodes.")
    ("nozero", "Do not return concepts with zero rank.")
    ("variants,r", "Write also concept variants in PPV.")
    ("control_line,l", "First line in PPV files is control.")
	("prefix,p", value<string>(), "Prefix added to all output ppv files.")
    ("ranks_nonorm", "Do not normalize ranks even with topK or threshold cuts.")
    ;

  options_description po_desc_server("Client/Server options");
  po_desc_server.add_options()
    ("daemon", "Start a daemon listening to port. Assumes --port")
    ("port", value<unsigned int>(), "Port to listen/send information.")
    ("client", "Use client mode to send contexts to the ukb daemon. Bare in mind that the configuration is that of the server.")
    ("shutdown", "Shutdown ukb daemon.")
	;

  options_description po_hidden("Hidden");
  po_hidden.add_options()
    ("only_ctx_words,C", "Backward compatibility with -C.")
    ("only_synsets", "Output only (normalized) PPVs for synsets.")
    ("concept_graph,G", "Backward compatibility with -G.")
    ("input-file",value<string>(), "Input file.")
    ("output-file",value<string>(), "Output file.")
    ;

  options_description po_visible(desc_header);
  po_visible.add(po_desc).add(po_desc_input).add(po_desc_prank).add(po_desc_dict).add(po_desc_output).add(po_desc_server);

  options_description po_desc_all("All options");
  po_desc_all.add(po_visible).add(po_hidden);

  positional_options_description po_optdesc;
  po_optdesc.add("input-file", 1);
  po_optdesc.add("output-file", 1);

  try {
    variables_map vm;
    store(command_line_parser(argc, argv).
	  options(po_desc_all).
	  positional(po_optdesc).
	  run(), vm);
    notify(vm);

    // If asked for help, don't do anything more

    if (vm.count("help")) {
      cout << po_visible << endl;
      exit(0);
    }

    if (vm.count("version")) {
      cout << glVars::ukb_version << endl;
      exit(0);
    }

    // verbosity

    if (vm.count("verbose")) {
      glVars::verbose = 1;
      glVars::debug::warning = 1;
    }

    if (vm.count("kb_binfile")) {
      kb_binfile = vm["kb_binfile"].as<string>();
    }

    if (vm.count("nopos")) {
	  glVars::input::filter_pos = false;
    }

    if (vm.count("minput")) {
	  glVars::input::swallow = true;
    }

    if (vm.count("ctx_noweight")) {
	  glVars::input::weight = false;
    }

    if (vm.count("variants")) {
	  output_variants_ppv = true;
    }

    if (vm.count("control_line")) {
	  output_control_line = true;
    }

    if (vm.count("out_dir")) {
      out_dir = vm["out_dir"].as<string>();
    }

    if (vm.count("prefix")) {
      ppv_prefix = vm["prefix"].as<string>();
    }

    if (vm.count("dict_file")) {
      glVars::dict::text_fname = vm["dict_file"].as<string>();
    }

    if (vm.count("prank_weight")) {
	  glVars::prank::use_weight = true;
    }

    if (vm.count("dict_strict")) {
      glVars::dict::swallow = false;
    }

    if (vm.count("dict_weight")) {
	  glVars::dict::use_weight = true;
    }

    if (vm.count("dict_weight_smooth")) {
      glVars::dict::weight_smoothfactor = vm["dict_weight_smooth"].as<float>();
    }

    if (vm.count("static")) {
	  opt_static=true;
    }

    if (vm.count("nostatic")) {
	  glVars::csentence::disamb_minus_static = true;
    }

    if (vm.count("prank_iter")) {
	  iterations = vm["prank_iter"].as<size_t>();
	  check_convergence = true;
    }

    if (vm.count("prank_threshold")) {
	  thresh = vm["prank_threshold"].as<float>();
	  check_convergence = true;
    }

    if (vm.count("prank_damping")) {
	  float dp = vm["prank_damping"].as<float>();
	  if (dp <= 0.0 || dp > 1.0) {
		cerr << "Error: invalid prank_damping value " << dp << "\n";
		goto END;
	  }
      glVars::prank::damping = dp;
    }

    if (vm.count("trunc_ppv")) {
      trunc_ppv = vm["trunc_ppv"].as<float>();
    }

    if (vm.count("trunc_topK")) {
	  size_t topK = vm["trunc_topK"].as<size_t>();
	  if(!topK) {
		cerr << "Error: trunc_topK is zero\n.";
		goto END;
	  }
      trunc_ppv = topK;
    }

    if (vm.count("nozero")) {
	  opt_nozero = true;
    }

    if (vm.count("input-file")) {
      fullname_in = vm["input-file"].as<string>();
    }

    if (vm.count("altdict")) {
      alternative_dict_fname = vm["altdict"].as<string>();
    }

	if (vm.count("daemon")) {
	  opt_daemon = true;
	}

	if (vm.count("port")) {
	  port =  vm["port"].as<unsigned int>();
	}

	if (vm.count("client")) {
	  opt_client = true;
	}

	if (vm.count("shutdown")) {
	  opt_shutdown = true;
	}

  }
  catch(std::exception& e) {
    cerr << e.what() << "\n";
	exit(-1);
  }


  if(opt_shutdown) {
	if (client_stop_server(port)) {
	  cerr << "Stopped UKB daemon on port " << lexical_cast<string>(port) << "\n";
	  return 0;
	} else {
	  cerr << "Can not stop UKB daemon on port " << lexical_cast<string>(port) << "\n";
	  return 1;
	}
  }

  if (check_convergence) set_pr_convergence(iterations, thresh);

  // if not daemon, check input files (do it early before loading KB and dictionary)
  if (!fullname_in.size() and !opt_daemon) {
    cout << po_visible << endl;
    cout << "Error: No input" << endl;
    exit(-1);
  }

  // if not --client, load KB
  if (!opt_client) {
	try {
	  Kb::create_from_binfile(kb_binfile);
	} catch (std::exception & e) {
	  cerr << e.what() << "\n";
	  return 1;
	}
  }

  if (opt_static) {
	compute_static_ppv();
	goto END;
  }

  // if not --client, load dictionary
  if (!opt_client) {
	size_t dict_size = 0;
	try {
	  dict_size = WDict::instance().size();
	  if (alternative_dict_fname.size()) {
		WDict::instance().read_alternate_file(alternative_dict_fname);
	  }
	} catch (std::exception & e) {
	  cerr << e.what() << "\n";
	  return dict_size == 0;
	}
  }

  if (opt_daemon) {
	// accept malformed contexts, as we don't want the daemon to die.
	glVars::input::swallow = true;
	cout << "Starting UKB daemon on port " << lexical_cast<string>(port) << " ... ";
	return start_daemon(port, &load_kb_dict, &handle_server_read);
  }

  // create stream from input file
  if (fullname_in == "-" ) {
	// read from <STDIN>
    cmdline += " <STDIN>";
	fullname_in = "<STDIN>";
  } else {
	input_ifs.open(fullname_in.c_str(), ofstream::in);
	if (!input_ifs) {
	  cerr << "Can't open " << fullname_in << endl;
	  exit(-1);
	}
	// redirect std::cin to read from file
	std::cin.rdbuf(input_ifs.rdbuf());
  }

  if (opt_client) {
	return !client(std::cin, port, out_dir);
  }

  try {
	compute_sentence_vectors(std::cin, out_dir);
  } catch(std::exception& e) {
    cerr << "Errore reading " << fullname_in << "\n" << e.what() << "\n";
	exit(-1);
  }

 END:
  return 0;
}

/*
 * Local Variables:
 * mode: c++
 * compile-command: "make"
 * End:
 */
