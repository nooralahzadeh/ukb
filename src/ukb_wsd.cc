#include "wdict.h"
#include "common.h"
#include "globalVars.h"
#include "kbGraph.h"
#include "disambGraph.h"
#include "fileElem.h"
#include <string>
#include <iostream>
#include <fstream>
#include <syslog.h>

#include "ukbServer.h"

// Basename & friends
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

// Program options

#include <boost/program_options.hpp>

using namespace ukb;
using namespace std;
using namespace boost;

/////////////////////////////////////////////////////////////
// Global variables

string stra;
enum dgraph_rank_methods {
  dppr,
  dppr_w2w,
  ddegree,
  dstatic
};

enum dis_method {
  dgraph_bfs,
  dgraph_dfs,
  ppr,
  ppr_w2w,
  ppr_static
};

dgraph_rank_methods dgraph_rank_method = dstatic;
bool use_dfs_dgraph = false;

dis_method opt_dmethod = ppr;
string cmdline;
string alternative_dict_fname;
string kb_binfile; // The name of KB file
bool opt_daemon = false;

// Program options stuff

/* Auxiliary functions for checking input for validity. */

/* Function used to check that 'opt1' and 'opt2' are not specified
   at the same time. */
void conflicting_options(const boost::program_options::variables_map& vm,
                         const char* opt1, const char* opt2)
{
  if (vm.count(opt1) && !vm[opt1].defaulted()
      && vm.count(opt2) && !vm[opt2].defaulted())
    throw logic_error(string("Conflicting options '")
					  + opt1 + "' and '" + opt2 + "'.");
}

/* Function used to check that of 'for_what' is specified, then
   'required_option' is specified too. */
void option_dependency(const boost::program_options::variables_map& vm,
					   const char* for_what, const char* required_option)
{
  if (vm.count(for_what) && !vm[for_what].defaulted())
    if (vm.count(required_option) == 0 || vm[required_option].defaulted())
      throw logic_error(string("Option '") + for_what
						+ "' requires option '" + required_option + "'.");
}

///////////////////////////////////////

// Main functions

// Disambiguate using disambiguation graph (dgraph) method

bool rank_dgraph(const CSentence & cs,
				 DisambGraph & dg,
				 vector<float> & ranks) {

  bool ok = false;
  switch(dgraph_rank_method) {
  case dppr:
  case dppr_w2w:
	ok = csentence_dgraph_ppr(cs, dg, ranks);
	break;
  case ddegree:
	ok = dgraph_degree(dg, ranks);
	break;
  case dstatic:
	ok = dgraph_static(dg, ranks);
	break;
  }
  return ok;
}


void fill_dgraph(CSentence & cs, DisambGraph & dgraph) {

  if (use_dfs_dgraph) {
	if (glVars::dGraph::stopCosenses)
	  fill_disamb_graph_dfs_nocosenses(cs, dgraph);
	else fill_disamb_graph_dfs(cs, dgraph);
  }
  else fill_disamb_graph(cs, dgraph);

}

void disamb_dgraph_from_corpus_w2w(CSentence & cs) {

  // fall back to static if csentence has only one word
  if (cs.size() == 1) {
	if (glVars::debug::warning)
	  cerr << "dis_csent: using static for context " << cs.id() << endl;
	const vector<float> ranks = Kb::instance().static_prank();
	disamb_csentence_kb(cs, ranks);
  } else {
	DisambGraph dgraph;
	fill_dgraph(cs, dgraph);
	vector<float> ranks;
	for(CSentence::iterator cw_it = cs.begin(), cw_end = cs.end();
		cw_it != cw_end; ++cw_it) {
	  if(!cw_it->is_tgtword()) continue;
	  bool ok = csentence_dgraph_ppr(cs, dgraph, ranks, cw_it);
	  if (!ok) {
		std::cerr << "Error when calculating ranks for sentence " << cs.id() << "\n";
		std::cerr << "(No word links to KB ?)\n";
		return;
	  }
	  disamb_cword_dgraph(cw_it, dgraph, ranks);
	}
  }
}

void disamb_dgraph_from_corpus(CSentence & cs) {

  if (dgraph_rank_method == dppr_w2w) {
	disamb_dgraph_from_corpus_w2w(cs);
	return;
  }

  // fall back to static if csentence has only one word
  if (cs.size() == 1) {
	if (glVars::debug::warning)
	  cerr << "dis_csent: using static for context " << cs.id() << endl;
	const vector<float> ranks = Kb::instance().static_prank();
	disamb_csentence_kb(cs, ranks);
  } else {
	DisambGraph dgraph;
	fill_dgraph(cs, dgraph);
	vector<float> ranks;
	bool ok = rank_dgraph(cs, dgraph, ranks);
	if (!ok) {
	  cerr << "Error when calculating ranks for sentence " << cs.id() << "\n";
	  cerr << "(No word links to KB ?)\n";
	  return;
	}
	disamb_csentence_dgraph(cs, dgraph, ranks);
  }
}

void ppr_csent(CSentence & cs) {

  vector<float> ranks;
  bool ok = calculate_kb_ppr(cs,ranks);
  if (!ok) {
	cerr << "Unknown error when calculating ranks for sentence " << cs.id() << "\n";
	return;
  }
  disamb_csentence_kb(cs, ranks);
}

void dis_csent_classic_prank(CSentence &cs) {

  static vector<float> ranks;
  if (!ranks.size()) {
	ranks = Kb::instance().static_prank();
  }
  disamb_csentence_kb(cs, ranks);
}


void dispatch_run_cs(CSentence & cs) {

  switch(opt_dmethod) {
  case dgraph_bfs:
	use_dfs_dgraph = false; // use bfs
	disamb_dgraph_from_corpus(cs);
	break;
  case dgraph_dfs:
	use_dfs_dgraph = true;
	disamb_dgraph_from_corpus(cs);
	break;
  case ppr:
	ppr_csent(cs);
	break;
  case ppr_w2w:
	calculate_kb_ppr_by_word_and_disamb(cs);
	break;
  case ppr_static:
	dis_csent_classic_prank(cs);
	break;
  };
}

void dispatch_run(istream & is, ostream & os) {

  size_t l_n = 0;

  CSentence cs;
  while (cs.read_aw(is, l_n)) {
	dispatch_run_cs(cs);
	cs.print_csent(os);
	cs = CSentence();
  }
}

///////////////////////////////////////////////
// Server/clien functions

// Return FALSE means kill server

bool handle_server_read(sSession & session) {
  string ctx_id;
  string ctx;
  try {
	session.receive(ctx);
	if (ctx == "stop") return false;
	session.send(cmdline);
	while(1) {
	  if (!session.receive(ctx_id)) break;
	  if (!session.receive(ctx)) break;
	  CSentence cs(ctx_id, ctx);
	  dispatch_run_cs(cs);
	  ostringstream oss;
	  cs.print_csent(oss);
	  string oss_str(oss.str());
	  if (!oss_str.length()) oss_str = "#"; // special line if not output
	  session.send(oss_str);
	}
  } catch (std::exception& e)	{
	// send error and close the session.
	// Note: the server is still alive for new connections.
	session.send(e.what());
  }
  return true;
}

bool client(istream & is, ostream & os, unsigned int port) {
  // connect to ukb port and send data to it.
  sClient client("localhost", port);
  string server_cmd;
  string go("go");
  if (client.error()) {
	std::cerr << "Error when connecting: " << client.error_str() << std::endl;
	return false;
  }
  string id, ctx, out;
  size_t l_n = 0;
  try {
	client.send(go);
	client.receive(server_cmd);
	os << server_cmd << std::endl;
	while(read_line_noblank(is, id, l_n)) {
	  if(!read_line_noblank(is, ctx, l_n)) return false;
	  client.send(id);
	  client.send(ctx);
	  client.receive(out);
	  if (out == "#") continue; // empty output for that context
	  os << out;
	  os.flush();
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

void load_kb_and_dict() {

  size_t dict_size = 0;
  if (opt_daemon) {
	string aux("Loading KB ");
	aux += kb_binfile;
	syslog(LOG_INFO | LOG_USER, aux.c_str());
  } else if (glVars::verbose) {
	cout << "Loading KB " + kb_binfile + "\n";
  }
  Kb::create_from_binfile(kb_binfile);
  // Explicitly load dictionary only if:
  // - there is a dictionary name (textual or binary)
  // - opt_daemon is set
  if (!opt_daemon) return;
  if (!(glVars::dict::text_fname.size() + glVars::dict::bin_fname.size())) return;
  string aux("Loading Dict ");
  aux += glVars::dict::text_fname.size() ? glVars::dict::text_fname : glVars::dict::bin_fname;
  syslog(LOG_INFO | LOG_USER, aux.c_str());
  // looking for "fake_entry" causes dictionary to be loaded in memory
  WDict_entries fake_entry = WDict::instance().get_entries("kaka", "");
  if (alternative_dict_fname.size()) {
	WDict::instance().read_alternate_file(alternative_dict_fname);
  }
  if (!fake_entry.size()) return;
}

void test() {

  size_t l_n = 0;

  glVars::dict::use_shuffle = false;
  CSentence cs;
  cs.read_aw(cin, l_n);
  cs.debug(cerr);
}

int main(int argc, char *argv[]) {


  map<string, dgraph_rank_methods> map_dgraph_ranks;

  map_dgraph_ranks["ppr"] = dppr;
  map_dgraph_ranks["ppr_w2w"] = dppr_w2w;
  map_dgraph_ranks["degree"] = ddegree;
  map_dgraph_ranks["static"] = dstatic;

  bool opt_do_test = false;
  bool opt_client = false;
  bool opt_shutdown = false;

  cmdline = string("!! -v ");
  cmdline += glVars::ukb_version;
  for (int i=0; i < argc; ++i) {
    cmdline += " ";
    cmdline += argv[i];
  }

  vector<string> input_files;
  string fullname_in;
  ifstream input_ifs;

  unsigned int port = 10000;
  size_t iterations = 0;
  float thresh = 0.0;
  bool check_convergence = false;

  using namespace boost::program_options;

  const char desc_header[] = "ukb_wsd: perform WSD with KB based algorithm\n"
    "Usage examples:\n"
	"ukb_wsd -D dict.txt -K kb.bin --ppr input.txt    -> Disambiguate input.txt using PPR technique according to graph kb.bin and dictionary dict.txt\n"
    "ukb_wsd -D dict.txt -K kb.bin --dgraph_dfs input.txt -> Disambiguate input.txt using disambiguation graph technique, according to graph kb.bin and dictionary dict.txt\n"
	"Options";

  //options_description po_desc(desc_header);

  options_description po_desc("General");

  po_desc.add_options()
    ("help,h", "This page")
    ("version", "Show version.")
    ("kb_binfile,K", value<string>(), "Binary file of KB (see compile_kb).")
    ("dict_file,D", value<string>(), "Dictionary text file.")
    ("dict_binfile", value<string>(), "Dictionary binary file.")
    ;

  options_description po_desc_wsd("WSD methods");
  po_desc_wsd.add_options()
    ("ppr", "Given a text input file, disambiguate context using Personalized PageRank method.")
    ("ppr_w2w", "Given a text input file, disambiguate context using Personalized PageRank method word by word (see README).")
    ("static", "Given a text input file, disambiguate context using static pageRank over kb.")
    ("dgraph_bfs", "Given a text input file, disambiguate context using disambiguation graph mehod (bfs).")
    ("dgraph_dfs", "Given a text input file, disambiguate context using disambiguation graph mehod (dfs).")
    ("nostatic", "Substract static ppv to final ranks.")
    ("noprior", "Don't multiply priors to target word synsets if --ppr_w2w and --dict_weight are selected.")
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
    ("prank_threshold", value<float>(), "Threshold for stopping PageRank. Default is zero. Good value is 0.0001.")
    ("prank_damping", value<float>(), "Set damping factor in PageRank equation. Default is 0.85.")
    ("dgraph_rank", value<string>(), "Set disambiguation method for dgraphs. Options are: static(default), ppr, ppr_w2w, degree.")
    ("dgraph_maxdepth", value<size_t>(), "If --dgraph_dfs is set, specify the maximum depth (default is 6).")
    ("dgraph_nocosenses", "If --dgraph_dfs, stop DFS when finding one co-sense of target word in path.")
    ;

  options_description po_desc_dict("Dictionary options");
  po_desc_dict.add_options()
	("altdict", value<string>(), "Provide an alternative dictionary overriding the values of default dictionary.")
    ("dict_weight", "Use weights when linking words to concepts (dict file has to have weights).")
    ("smooth_dict_weight", value<float>(), "Smoothing factor to be added to every weight in dictionary concepts. Default is 1.")
    ("dict_strict", "Be strict when reading the dictionary and stop when any error is found.")
    ;

  options_description po_desc_output("Output options");
  po_desc_output.add_options()
    ("allranks", "Write key file with all synsets associated with ranks.")
    ("verbose,v", "Be verbose.")
    ("no-monosemous", "Don't output anything for monosemous words.")
    ("ties", "Output also in case of ties.")
    ("rank_nonorm", "Do not normalize the ranks of target words.")
	;

  options_description po_desc_server("Client/Server options");
  po_desc_server.add_options()
    ("daemon", "Start a daemon listening to port. Assumes --port")
    ("port", value<unsigned int>(), "Port to listen/send information.")
    ("client", "Use client mode to send contexts to the ukb daemon. Bare in mind that the configuration is that of the server.")
    ("shutdown", "Shutdown ukb daemon.")
	;

  options_description po_visible(desc_header);
  po_visible.add(po_desc).add(po_desc_wsd).add(po_desc_prank).add(po_desc_input).add(po_desc_dict).add(po_desc_output).add(po_desc_server);

  options_description po_hidden("Hidden");
  po_hidden.add_options()
    ("bcomp_kb_binfile,M", value<string>(), "Backward compatibility with -K.")
    ("bcomp_dictfile,W", value<string>(), "Backward compatibility with -D.")
    ("only_ctx_words,C", "Backward compatibility with -C.")
    ("concept_graph,G", "Backward compatibility with -G.")
    ("dgraph", "Backward compatibility with --dgraph.")
    ("test,t", "(Internal) Do a test.")
    ("input-file",value<string>(), "Input file.")
    ;
  options_description po_all("All options");
  po_all.add(po_visible).add(po_hidden);

  positional_options_description po_optdesc;
  po_optdesc.add("input-file", 1);
  //    po_optdesc.add("output-file", 1);

  try {

    variables_map vm;
    store(command_line_parser(argc, argv).
		  options(po_all).
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

    if (vm.count("nopos")) {
	  glVars::input::filter_pos = false;
    }

    if (vm.count("minput")) {
	  glVars::input::swallow = true;
    }

    if (vm.count("ctx_noweight")) {
	  glVars::input::weight = false;
    }

    if (vm.count("ppr")) {
	  opt_dmethod = ppr;
    }

    if (vm.count("ppr_w2w")) {
	  opt_dmethod = ppr_w2w;
    }

    if (vm.count("static")) {
	  opt_dmethod = ppr_static;
    }

    if (vm.count("dgraph_bfs")) {
	  opt_dmethod = dgraph_bfs;
    }

    if (vm.count("dgraph")) {
	  opt_dmethod = dgraph_bfs;
    }

    if (vm.count("dgraph_dfs")) {
	  opt_dmethod = dgraph_dfs;
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
		exit(-1);
	  }
      glVars::prank::damping = dp;
    }

	if (vm.count("dgraph_maxdepth")) {
	  size_t md = vm["dgraph_maxdepth"].as<size_t>();
	  if (md == 0) {
		cerr << "Error: invalid dgraph_maxdepth of zero\n";
		exit(-1);
	  }
	  glVars::dGraph::max_depth = md;
	}

	if (vm.count("dgraph_nocosenses")) {
	  glVars::dGraph::stopCosenses = true;
	}

	if (vm.count("dgraph_rank")) {
      string str = vm["dgraph_rank"].as<string>();
	  map<string, dgraph_rank_methods>::iterator it = map_dgraph_ranks.find(str);
	  if (it == map_dgraph_ranks.end()) {
		cerr << "Error: invalid dgraph_rank method. Should be one of: ";
		for(map<string, dgraph_rank_methods>::iterator iit = map_dgraph_ranks.begin();
			iit != map_dgraph_ranks.end(); ++iit)
		  cerr << " " << iit->first;
		cerr << "\n";
		exit(-1);
	  }
	  dgraph_rank_method = it->second;
	}

    if (vm.count("nostatic")) {
	  glVars::csentence::disamb_minus_static = true;
    }

    if (vm.count("noprior")) {
	  glVars::csentence::mult_priors = false;
    }

    if (vm.count("bcomp_dictfile")) {
      glVars::dict::text_fname = vm["bcomp_dictfile"].as<string>();
    }

    if (vm.count("dict_file")) {
      glVars::dict::text_fname = vm["dict_file"].as<string>();
    }

    if (vm.count("dict_binfile")) {
      glVars::dict::bin_fname = vm["dict_binfile"].as<string>();
    }

    if (vm.count("dict_strict")) {
      glVars::dict::swallow = false;
    }

    if (vm.count("dict_weight")) {
      glVars::dict::use_weight = true;
    }

    if (vm.count("smooth_dict_weight")) {
      glVars::dict::weight_smoothfactor = vm["smooth_dict_weight"].as<float>();
    }

    if (vm.count("bcomp_kb_binfile")) {
      kb_binfile = vm["bcomp_kb_binfile"].as<string>();
    }

    if (vm.count("kb_binfile")) {
      kb_binfile = vm["kb_binfile"].as<string>();
    }

    if (vm.count("rank_alg")) {
      glVars::RankAlg alg = glVars::get_algEnum(vm["rank_alg"].as<string>());
      if (alg == glVars::no_alg) {
		cerr << "Error: Undefined rank algorithm " << vm["rank_alg"].as<string>() << endl;
		exit(-1);
      }
      glVars::rAlg = alg;
    }

    if (vm.count("prank_weight")) {
	  glVars::prank::use_weight = true;
    }

    if (vm.count("input-file")) {
      fullname_in = vm["input-file"].as<string>();
    }

    if (vm.count("verbose")) {
      glVars::verbose = 1;
      glVars::debug::warning = 1;
    }

    if (vm.count("allranks")) {
      glVars::output::allranks = true;
    }

    if (vm.count("test")) {
      opt_do_test = true;
    }

    if (vm.count("no-monosemous")) {
      glVars::output::monosemous = false;
    }

    if (vm.count("ties")) {
      glVars::output::ties = true;
    }

    if (vm.count("rank_nonorm")) {
      glVars::output::norm_ranks = false;
    }

    if (vm.count("altdict")) {
      alternative_dict_fname = vm["altdict"].as<string>();
    }

	if (vm.count("daemon")) {
	  opt_daemon = true;
	}

	if (vm.count("port")) {
	  port = vm["port"].as<unsigned int>();;
	}

	if (vm.count("client")) {
	  opt_client = true;
	}

	if (vm.count("shutdown")) {
	  opt_shutdown = true;
	}

  } catch(std::exception& e) {
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

  // if not daemon, check input files (do it early before loading KB and dictionary)
  if (!fullname_in.size() and !opt_daemon) {
    cout << po_visible << endl;
    cout << "Error: No input" << endl;
    exit(-1);
  }

  if (check_convergence) set_pr_convergence(iterations, thresh);

  // if --daemon, fork server process (has to be done before loading KB and dictionary)
  if (opt_daemon) {
	try {
	  // Get absolute names of KB and dict
	  kb_binfile =  get_fname_absolute(kb_binfile);
	  glVars::dict::text_fname = get_fname_absolute(glVars::dict::text_fname);
	  alternative_dict_fname = get_fname_absolute(alternative_dict_fname);
	} catch(std::exception& e) {
	  cerr << e.what() << "\n";
	  return 1;
	}
	// accept malformed contexts, as we don't want the daemon to die.
	glVars::input::swallow = true;
	cout << "Starting UKB daemon on port " << lexical_cast<string>(port) << " ... \n";
	if (!kb_binfile.size()) {
	  cerr << "Error: no KB file\n";
	  return 1;
	}
	return start_daemon(port, &load_kb_and_dict, &handle_server_read);
  }

  // if not --client, load KB and dictionary
  if (!opt_client) {
	if (!kb_binfile.size()) {
	  cerr << "Error: no KB file\n";
	  exit(1);
	}
	try {
	  load_kb_and_dict();
	} catch (std::exception & e) {
	  cerr << e.what() << "\n";
	  return 1;
	}
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
	// TODO :
	// - check parameters
	// - cmdline
	return !client(std::cin, std::cout, port);
  }

  if (opt_do_test) {
	test();
	return 0;
  }

  cout << cmdline << "\n";

  try {
	dispatch_run(std::cin, std::cout);
  } catch (std::exception & e) {
	cerr << "Error reading " << fullname_in << "\n" << e.what() << "\n";
	return 0;
  }

  return 0;
}
