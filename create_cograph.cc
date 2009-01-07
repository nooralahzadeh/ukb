#include "common.h"
#include "globalVars.h"
#include "fileElem.h"
#include "coocGraph.h"
#include "hlex_agtree.h"

#include <string>
#include <iostream>
#include <fstream>

// graphviz

#include <boost/graph/graphviz.hpp>
#include <boost/graph/filtered_graph.hpp>

// Program options

#include <boost/program_options.hpp>

using namespace std;
using namespace boost;
using namespace ukb;

// global vars

bool opt_verbose = false;


void chsq (const string & input_name,
	   const string & output_name,
	   bool normalize) {

  CoocGraph coog;

  coog.read_from_binfile(input_name);
  coog.calculate_chisq();
  if(normalize) {
    if (!coog.normalize_edge_freqs()) {
      cerr << "Error: can't normalize freqs." << endl;
    }
  }
  coog.prune_zero_edges();
  coog.write_to_binfile(output_name);
}


void query (const string & coName, const string & str) {

  CoocGraph coog;
  coog.read_from_binfile(coName);

  bool aux;
  CoocGraph::vertex_descriptor u;
  
  tie(u, aux) = coog.get_vertex_by_name(str);
  if (aux) {
    write_vertex(cout, u, coog.graph());
    cout << "\n";
    CoocGraph::out_edge_iterator it , end;
    tie(it, end) = out_edges(u, coog.graph());
    for(;it != end; ++it) {
      cout << "  ";
      write_vertex(cout, target(*it, coog.graph()), coog.graph());
      cout << " " << get(edge_freq, coog.graph(), *it) << "\n";
    }
  }
}

void create_from_dlin(const vector<string> & input_files, 
		      const string & graph_binfile) {

  CoocGraph coog;
  if (glVars::verbose) 
    cerr << "Creating from dling th." << endl;

  for(vector<string>::const_iterator it=input_files.begin();
      it != input_files.end(); ++it) {
    string word = basename(*it);
    ifstream fh(it->c_str());
    if (!fh) {
      cerr << "Error: can't read " << *it << endl;
      exit(-1);
    }

    // Fill dling
    coog.fill_dling_th(fh);
    // First line is word, second similarities. F. ex.
    //
    // a
  }
  //coog.remove_isolated_vertices(1); // Remove dangling nodes
  coog.write_to_binfile(graph_binfile);
}



void info (string & coName) {

  CoocGraph coog;
  float min, max;

  coog.read_from_binfile(coName);
  size_t edge_n = coog.num_edges();
  tie(min, max) = coog.minmax();
  if (edge_n & 2) edge_n++; 
  cout << "V:" << coog.num_vertices() << " E:" << edge_n/2 << " Docs:" << coog.num_docs() << endl;
  cout << "min freq:" << min << " max freq:" << max << endl;
}

void histogram (string & coName) {

  CoocGraph coog;

  coog.read_from_binfile(coName);
  CoocGraph::boost_graph_t & g = coog.graph();

  CoocGraph::vertex_iterator it, end;
  tie(it, end) = vertices(g);
  for(;it != end; ++it) {
    cout << out_degree(*it, g) << "\n";
  }
}

void edge_histogram (string & coName) {

  CoocGraph coog;

  coog.read_from_binfile(coName);
  CoocGraph::boost_graph_t & g = coog.graph();

  CoocGraph::edge_iterator it, end;
  tie(it, end) = edges(g);
  for(;it != end; ++it) {
    cout << get(edge_freq, g, *it) << "\n";
  }
}

void test(string & coName) {

  CoocGraph coog;

  coog.read_from_binfile(coName);

  ofstream fo("kk.dot", ofstream::out);
  if (!fo) {
    cerr << "Can't create kk.dot\n";
    exit(-1);
  }

  boost::write_graphviz(fo, 
			coog.graph(),
			make_my_writer(get(vertex_name, coog.graph()),"w"));
  
//   CoocGraph::vertex_iterator it, end;
//   for(tie(it, end) = vertices(coog.graph()); it != end; ++it) {
//     if(out_degree(*it, coog.graph()) == 0) {
//       cout << get(vertex_name, coog.graph(), *it) << '\n';
//     }
//   }
}

int main(int argc, char *argv[]) {

  srand(3);

  //bool opt_force = false;

  bool opt_query = false;
  bool opt_info = false;
  bool opt_histo = false;
  bool opt_edge_histo = false;
  bool opt_test = false;
  bool opt_chsq = false;
  bool opt_dlin = false;
  bool opt_normalize = false;

  string query_vertex;

  string fullname_in;
  string graph_binfile("coocgraph.bin");


  //float threshold = 0.0; // 95.0% confidence
  //float threshold = 2.706; // 80.0% confidence
  //float threshold = 3.84146; // 95.0% confidence
  //float threshold = 5.41189; // 98.0% confidence
  //float threshold = 6.6349;  // 99.0% confidence
  //float threshold = 10.827;  // 99.99% confidence


  const char desc_header[] = "create_cograph: create a coocurrence graph\n"
    "Usage:\n"
    "create_cograph [-o ouput.bin] file_or_dir \n"
    "\t file_or_dir: input textfile or directory. If directory given, all input files are read.\n"
    "\t -o Name of serialization graph. Default is coocgraph.bin.\n"
    "create_cograph [-o cograph.bin] --dling file_or_dir \n"
    "\t create a cograph using as input Dekang-Lin thesaurus." 
    "create_cograph [-t threshold] [-o output.bin] -c input.bin\n"
    "Options:";
  
  using namespace boost::program_options;

  options_description po_desc(desc_header);

  po_desc.add_options()
    ("dling,d", "Create a cograph using as input Dekang-Lin thesaurus." )
    ("chsq,c", "Given a serialized graph, create another with chisq information. Prune it according threshold (see -t).")
    ("edge_histogram,E", "Output edge freq. values.")
    ("help,h", "This help page.")
    ("histogram,H", "Output vertex degrees.")
    ("normalize,N", "Normalize chisq. values, so they fit in [0,1] interval.")
    ("info,i", "Get info of a serielized cograph.")
    //("force,f", "Don't use previous coocgraph.bin if exists.")
    ("query,q", value<string>(), "Given a vertex name, display its coocurrences.")
    ("output,o", value<string>(), "Output filename (default is ./coocgraph.bin).")
    ("verbose,v", "Be verbose.")
    ("threshold,t", value<float>(), 
     "Set a threshold for prunning the graph (use with -c option).\n"
     "\tTypical threshold values:\n"
     "\t  0.708   -- 60.0%  confidence\n"
     "\t  1.642   -- 80.0%  confidence\n"
     "\t  2.706   -- 90.0%  confidence\n"
     "\t  3.84146 -- 95.0%  confidence\n"
     "\t  5.41189 -- 98.0%  confidence\n "
     "\t  6.6349  -- 99.0%  confidence\n"
     "\t  7.883   -- 99.5%  confidence\n"
     "\t  10.827  -- 99.99% confidence\n")
    ("test,T", "Internal test.")
    ;
  options_description po_desc_hide("Hidden");
  po_desc_hide.add_options()
    ("input-file",value<string>(), "Input file.")
    ;
  options_description po_desc_all("All options");
  po_desc_all.add(po_desc).add(po_desc_hide);
  
  positional_options_description po_optdesc;
  po_optdesc.add("input-file", 1);
  
  variables_map vm;

  try {
    store(command_line_parser(argc, argv).
	  options(po_desc_all).
	  positional(po_optdesc).
	  run(), vm);
    notify(vm);

    // If asked for help, don't do anything more

    if (vm.count("help")) {
      cout << po_desc << endl;
      exit(0);
    }

    if (vm.count("dling")) {
      opt_dlin = true;
    }

    if (vm.count("chsq")) {
      opt_chsq = true;
    }

    if (vm.count("info")) {
      opt_info = true;
    }

    if (vm.count("histogram")) {
      opt_histo = true;
    }

    if (vm.count("normalize")) {
      opt_normalize = true;
    }

    if (vm.count("edge_histogram")) {
      opt_edge_histo = true;
    }

//     if (vm.count("force")) {
//       opt_force = true;
//     }

    // verbosity
    if (vm.count("verbose")) {
      glVars::verbose = 1;
    }

    if (vm.count("test")) {
      opt_test = 1;
    }

    if (vm.count("query")) {
      opt_query = true;
      query_vertex = vm["query"].as<string>();
    }

    if (vm.count("threshold")) {
      glVars::chsq::threshold = vm["threshold"].as<float>();
    }

    if (vm.count("input-file")) {
      fullname_in = vm["input-file"].as<string>();
    }

    if (vm.count("output")) {
      graph_binfile = vm["output"].as<string>();
    }

  }
  catch(exception& e) {
    cerr << e.what() << "\n";
    throw(e);
  }


//______________________________________________________________________________

  CoocGraph coog;
  vector<string> input_files;

  input_files = extract_input_files(fullname_in);
  if(input_files.empty()) {
    cout << po_desc << endl;
    cerr << "Error: No input files." << endl;
    return -1;
  }

  if (opt_dlin) {
    create_from_dlin(input_files, graph_binfile);
    return 0;
  }

  if (opt_test) {
    test(input_files[0]);
    return 0;
  }

  if (opt_info) {
    info(input_files[0]);
    return 0;
  }

  if (opt_histo) {
    histogram(input_files[0]);
    return 0;
  }

  if (opt_edge_histo) {
    edge_histogram(input_files[0]);
    return 0;
  }

  if (opt_query) {
    query(input_files[0], query_vertex);
    return 0;
  }

  if (opt_chsq) {
    if(exists_file(graph_binfile)) {
      cerr << "Error: " << graph_binfile << " exists. Won't override."<< endl;
      return -1;
    }
    if (glVars::chsq::threshold < 0.0) {
      cerr << "Error: negative threshold\n";
      return -1;
    }
    if (glVars::verbose) {
      cout << "Cooc min: " << glVars::chsq::cooc_min << " Threshold: " << glVars::chsq::threshold << '\n';
    }
    chsq(input_files[0], graph_binfile, opt_normalize);
    return 0;
  }

//   if (!opt_force && exists_file(graph_binfile)) {
//     // Read previous graph
//     coog.read_from_binfile(graph_binfile);
//   }
  

//   if (opt_verbose) {
//     cout << "Before processing graph:\n";
//     cout << "V:" << coog.num_vertices() << " E:" << coog.num_edges() << " Docs:" << coog.num_docs() << endl;
//   }

  for(vector<string>::iterator it=input_files.begin();
      it != input_files.end(); ++it) {
    string word = basename(*it);
    ifstream fh(it->c_str());
    if (!fh) {
      cerr << "Error: can't read " << *it << endl;
      exit(-1);
    }
    if (opt_verbose) {
      cout << "Processing word " << word << " file " << *it << endl;
    }
    coog.fill_cograph(fh);
  }

  coog.write_to_binfile(graph_binfile);
  if (opt_verbose) {
    cout << "After processing graph:\n";
    cout << "V:" << coog.num_vertices() << " E:" << coog.num_edges() << " Docs:" << coog.num_docs() << endl;
  }
  return 0;
}


/*
 * Local Variables:
 * mode: c++
 * End:
 */
