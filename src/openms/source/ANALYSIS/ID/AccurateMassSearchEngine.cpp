// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2013.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Erhan Kenar $
// $Authors: Erhan Kenar, Chris Bielow $
// --------------------------------------------------------------------------

#include <OpenMS/ANALYSIS/ID/AccurateMassSearchEngine.h>
#include <OpenMS/CHEMISTRY/IsotopeDistribution.h>
#include <OpenMS/CHEMISTRY/EmpiricalFormula.h>
#include <OpenMS/CONCEPT/Constants.h>
#include <OpenMS/CHEMISTRY/IsotopeDistribution.h>
#include <OpenMS/DATASTRUCTURES/ListUtils.h>
#include <OpenMS/FORMAT/TextFile.h>

#include <OpenMS/SYSTEM/File.h>

#include <vector>
#include <map>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <fstream>

namespace OpenMS
{

  AccurateMassSearchEngine::AdductInfo_::AdductInfo_(const String& name, const EmpiricalFormula& adduct, int charge, bool is_intrinsic, uint mol_multiplier)
    : 
    name_(name),
    ef_(adduct),
    charge_(charge),
    is_intrinsic_(is_intrinsic), // true if compound just has a charge, but no actual adducts, e.g. 'M:2+'
    mol_multiplier_(mol_multiplier) 
  {
    if (charge_ == 0) throw Exception::InvalidParameter(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Charge of 0 is not allowed for an adduct (" + ef_.toString() + ")");
    mass_ = ef_.getMonoWeight();
  }

  double AccurateMassSearchEngine::AdductInfo_::getNeutralMass(double observed_mz) const
  {
    // decharge and remove adduct
    double mass = observed_mz * abs(charge_) - mass_;

    if (!is_intrinsic_)
    { // correct for electron masses
      double electrons_mass_diff(charge_ * Constants::ELECTRON_MASS_U);
      mass += electrons_mass_diff;
    }
    // the mol multiplier determines if we assume to be looking at dimers or higher
    // now, we just want to monomer, to compare its mass to a DB entry
    mass /= mol_multiplier_;

    return mass;
  }

  /// checks if an adduct (e.g.a 'M+2K-H;1+') is valid, i.e.a if the losses (==negative amounts) can actually be lost by the compound given in @p db_entry.
  /// If the negative parts are present in @p db_entry, true is returned.
  bool AccurateMassSearchEngine::AdductInfo_::isCompatible(EmpiricalFormula db_entry) const
  {
    return db_entry.contains(ef_ * -1);
  };

  int AccurateMassSearchEngine::AdductInfo_::getCharge() const
  {
    return charge_;
  }
    
  const String& AccurateMassSearchEngine::AdductInfo_::getName() const
  {
    return name_;
  }

  AccurateMassSearchEngine::AdductInfo_ AccurateMassSearchEngine::AdductInfo_::parseAdductString(const String adduct)
  {
    // adduct string looks like this:
    // M+2K-H;1+   or
    // 2M+CH3CN+Na;1+  (i.e. multimers are supported)
      
    // do some sanity checks on the string
      
    // retrieve adduct and charge
    String cp_str(adduct);
    cp_str.removeWhitespaces();
    StringList list;
    cp_str.split(";", list);
    // split term into formula and charge, e.g. "M-H" and "1-"
    String mol_formula, charge_str;
    bool intrinsic(false);
    if (list.size() == 2)
    {
      mol_formula = list[0];
      if (mol_formula == "M")
      {
        intrinsic = true;
      }
      charge_str = list[1];
    }
    else
    {
      throw Exception::InvalidValue(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Could not detect molecular ion;charge in '" + cp_str + "'. Got semicolon right?", cp_str);
    }

    // check if charge string is formatted correctly
    if ((!charge_str.hasSuffix("+")) && (!charge_str.hasSuffix("-")))
    {
      throw Exception::InvalidValue(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Charge sign +/- in the end of the string is missing! ", charge_str);
    }

    // get charge and sign (throws ConversionError if not an integer)
    int charge = charge_str.substr(0, charge_str.size() - 1).toInt();
      
    if (charge_str.suffix(1) == "+")
    {
      if (charge < 0)
      {
        charge *= -1;
      }
    }
    else
    {
      if (charge > 0)
      {
        charge *= -1;
      }
    }

    // not allowing double ++ or -- or +- or -+
    String op_str(mol_formula);
    op_str.substitute('-', '+');
    if (op_str.hasSubstring("++") || op_str.hasSuffix("+") || op_str.hasPrefix("+"))
    {
      throw Exception::InvalidValue(__FILE__, __LINE__, __PRETTY_FUNCTION__, "+/- operator must be surrounded by a chemical formula. Offending string: ", mol_formula);
    }

    // split by + and -
    op_str = mol_formula;
    if (op_str.has('%'))
    {
      throw Exception::InvalidValue(__FILE__, __LINE__, __PRETTY_FUNCTION__, "Character '%' not allowed within chemical formula. Offending string: ", mol_formula);
    }
    // ... we want to keep the - and +, so we add extra chars around, which we use as splitter later
    op_str.substitute("-", "%-%");
    op_str.substitute("+", "%+%");
    // split while keeping + and - as separate entries
    op_str.split("%", list);
      
    // some further sanity check if adduct formula is correct
    String m_part(list[0]);
    // std::cout << m_part.at(m_part.size() - 1) << std::endl;

    if (!m_part.hasSuffix("M"))
    {
      throw Exception::InvalidValue(__FILE__, __LINE__, __PRETTY_FUNCTION__, "First term of adduct string must contain the molecular entity 'M', optionally prefixed by a multiplier (e.g. '2M'); not found in ", m_part);
    }

    int mol_multiplier(1);
    // check if M has a multiplier in front
    if (m_part.length() > 1)
    { // will throw conversion error of not a number
      mol_multiplier = m_part.prefix(m_part.length()-1).toDouble();
    }

    // evaluate the adduct string ...
    // ... add/subtract each adduct compound
    bool op_plus(false);
    EmpiricalFormula ef;
    for (Size part_idx = 1 /* omit 0 index, since its 'M' */; part_idx < list.size(); ++part_idx)
    {
      if (list[part_idx] == "+")
      {
        op_plus = true;
        continue;
      }
      else if (list[part_idx] == "-")
      {
        op_plus = false;
        continue;
      }

      // std::cout << "putting " << tmpvec2[part_idx] << " into a formula with mass ";

      // check if formula has got a stoichiometry factor in front
      String formula_str(list[part_idx]);
      int stoichio_factor(1);
      int idx(0);
      while (isdigit(formula_str[idx])) ++idx;
      if (idx > 0)
      {
        stoichio_factor = formula_str.substr(0, idx).toInt();
        formula_str = formula_str.substr(idx, formula_str.size());
      }

      // std::cout << stoichio_factor << "*" << formula_str << " ";
      EmpiricalFormula ef_part(formula_str);
      // std::cout << part_formula.getMonoWeight() << std::endl;

      if (op_plus)
      {
        ef += ef_part * stoichio_factor;
      }
      else // "-" operator
      {
        ef -= ef_part * stoichio_factor;
      }
    }

    return AdductInfo_(cp_str, ef, charge, intrinsic, mol_multiplier);
  };


  /// default constructor
  AccurateMassSearchResult::AccurateMassSearchResult() :
    adduct_mass_(),
    query_mass_(),
    found_mass_(),
    charge_(),
    error_ppm_(),
    observed_rt_(),
    observed_intensity_(),
    individual_intensities_(),
    matching_index_(),
    source_feature_index_(),
    found_adduct_(),
    empirical_formula_(),
    matching_hmdb_ids_(),
    isotopes_sim_score_(-1.0)
  {

  }

  /// default destructor
  AccurateMassSearchResult::~AccurateMassSearchResult()
  {

  }

  /// copy constructor
  AccurateMassSearchResult::AccurateMassSearchResult(const AccurateMassSearchResult& source) :
    adduct_mass_(source.adduct_mass_),
    query_mass_(source.query_mass_),
    found_mass_(source.found_mass_),
    charge_(source.charge_),
    error_ppm_(source.error_ppm_),
    observed_rt_(source.observed_rt_),
    observed_intensity_(source.observed_intensity_),
    individual_intensities_(source.individual_intensities_),
    matching_index_(source.matching_index_),
    source_feature_index_(source.source_feature_index_),
    found_adduct_(source.found_adduct_),
    empirical_formula_(source.empirical_formula_),
    matching_hmdb_ids_(source.matching_hmdb_ids_),
    isotopes_sim_score_(source.isotopes_sim_score_)
  {

  }

  /// assignment operator
  AccurateMassSearchResult& AccurateMassSearchResult::operator=(const AccurateMassSearchResult& rhs)
  {
    if (this == &rhs)
      return *this;

    adduct_mass_ = rhs.adduct_mass_;
    query_mass_ = rhs.query_mass_;
    found_mass_ = rhs.found_mass_;
    charge_ = rhs.charge_;
    error_ppm_ = rhs.error_ppm_;
    observed_rt_ = rhs.observed_rt_;
    observed_intensity_ = rhs.observed_intensity_;
    individual_intensities_ = rhs.individual_intensities_;
    matching_index_ = rhs.matching_index_;
    source_feature_index_ = rhs.source_feature_index_;
    found_adduct_ = rhs.found_adduct_;
    empirical_formula_ = rhs.empirical_formula_;
    matching_hmdb_ids_ = rhs.matching_hmdb_ids_;
    isotopes_sim_score_ = rhs.isotopes_sim_score_;

    return *this;
  }

  double AccurateMassSearchResult::getAdductMass() const
  {
    return adduct_mass_;
  }

  void AccurateMassSearchResult::setAdductMass(const double& m)
  {
    adduct_mass_ = m;
  }

  double AccurateMassSearchResult::getQueryMass() const
  {
    return query_mass_;
  }

  void AccurateMassSearchResult::setQueryMass(const double& m)
  {
    query_mass_ = m;
  }

  double AccurateMassSearchResult::getFoundMass() const
  {
    return found_mass_;
  }

  void AccurateMassSearchResult::setFoundMass(const double& m)
  {
    found_mass_ = m;
  }

  Int AccurateMassSearchResult::getCharge() const
  {
    return charge_;
  }

  void AccurateMassSearchResult::setCharge(const Int& ch)
  {
    charge_ = ch;
  }

  double AccurateMassSearchResult::getErrorPPM() const
  {
    return error_ppm_;
  }

  void AccurateMassSearchResult::setErrorPPM(const double& ppm)
  {
    error_ppm_ = ppm;
  }

  double AccurateMassSearchResult::getObservedRT() const
  {
    return observed_rt_;
  }

  void AccurateMassSearchResult::setObservedRT(const double& rt)
  {
    observed_rt_ = rt;
  }

  double AccurateMassSearchResult::getObservedIntensity() const
  {
    return observed_intensity_;
  }

  void AccurateMassSearchResult::setObservedIntensity(const double& intensity)
  {
    observed_intensity_ = intensity;
  }

  std::vector<double> AccurateMassSearchResult::getIndividualIntensities() const
  {
    return individual_intensities_;
  }

  void AccurateMassSearchResult::setIndividualIntensities(const std::vector<double>& indiv_ints)
  {
    individual_intensities_ = indiv_ints;
  }

  Size AccurateMassSearchResult::getMatchingIndex() const
  {
    return matching_index_;
  }

  void AccurateMassSearchResult::setMatchingIndex(const Size& idx)
  {
    matching_index_ = idx;
  }

  Size AccurateMassSearchResult::getSourceFeatureIndex() const
  {
    return source_feature_index_;
  }

  void AccurateMassSearchResult::setSourceFeatureIndex(const Size& idx)
  {
    source_feature_index_ = idx;
  }

  const String& AccurateMassSearchResult::getFoundAdduct() const
  {
    return found_adduct_;
  }

  void AccurateMassSearchResult::setFoundAdduct(const String& add)
  {
    found_adduct_ = add;
  }

  const String& AccurateMassSearchResult::getFormulaString() const
  {
    return empirical_formula_;
  }

  void AccurateMassSearchResult::setEmpiricalFormula(const String& ep)
  {
    empirical_formula_ = ep;
  }

  const std::vector<String>& AccurateMassSearchResult::getMatchingHMDBids() const
  {
    return matching_hmdb_ids_;
  }

  void AccurateMassSearchResult::setMatchingHMDBids(const std::vector<String>& match_ids)
  {
    matching_hmdb_ids_ = match_ids;
  }

  double AccurateMassSearchResult::getIsotopesSimScore() const
  {
    return isotopes_sim_score_;
  }

  void AccurateMassSearchResult::setIsotopesSimScore(const double& sim_score)
  {
    isotopes_sim_score_ = sim_score;
  }

  void AccurateMassSearchResult::outputResults() const
  {
    std::cout << "adduct_mass: " << std::setprecision(8) << adduct_mass_ << "\n";
    std::cout << "query_mass: " << query_mass_ << "\n";
    std::cout << "found_mass: " << found_mass_ << "\n";
    std::cout << "charge: " << charge_ << "\n";
    std::cout << "error ppm: " << error_ppm_ << "\n";
    std::cout << "observed rt: " << observed_rt_ << "\n";
    std::cout << "observed intensity: " << observed_intensity_ << "\n";


    std::cout << "matching idx: " << matching_index_ << "\n";

    std::cout << "found_adduct_: " << found_adduct_ << "\n";
    std::cout << "emp. formula: " << empirical_formula_ << "\n";
    std::cout << "matching HMDB ids:";

    for (Size i = 0; i < matching_hmdb_ids_.size(); ++i)
    {
      std::cout << " " << matching_hmdb_ids_[i];
    }

    std::cout << "\n";
    std::cout << "isocheck sim score: " << isotopes_sim_score_ << std::endl; // ensure endl used at the end (but not before! performance!)
  }

  AccurateMassSearchEngine::AccurateMassSearchEngine() :
    DefaultParamHandler("AccurateMassSearchEngine"),
    ProgressLogger(),
    is_initialized_(false)
  {
    defaults_.setValue("mass_error_value", 5.0, "Tolerance allowed for accurate mass search.");

    defaults_.setValue("mass_error_unit", "ppm", "Unit of mass error (ppm or Da)");
    defaults_.setValidStrings("mass_error_unit", ListUtils::create<String>(("ppm,Da")));

    defaults_.setValue("ionization_mode", "positive", "Positive or negative ionization mode? If 'auto' is used, the first feature of the input map must contain the meta-value 'scan_polarity'. If its missing, the tool will exit with error.");
    defaults_.setValidStrings("ionization_mode", ListUtils::create<String>(("positive,negative,auto")));

    defaults_.setValue("isotopic_similarity", "false", "Computes a similarity score for each hit (only if the feature exhibits at least two isotopic mass traces).");
    defaults_.setValidStrings("isotopic_similarity", ListUtils::create<String>(("false,true")));

    defaults_.setValue("report_mode", "all", "Results are reported in one of several modes: Either (all) matching hits, the (top3) scoring hits, or the (best) scoring hit.");
    defaults_.setValidStrings("report_mode", ListUtils::create<String>(("all,top3,best")));

    defaults_.setValue("db:mapping", "CHEMISTRY/HMDBMappingFile.tsv", "Database input file, containing three tab-separated columns of mass, formula, identifier. "
                                                                      "If 'mass' is 0, it is re-computed from the molecular sum formula. "
                                                                      "By default CHEMISTRY/HMDBMappingFile.tsv in OpenMS/share is used! If empty, the default will be used.");
    defaults_.setValue("db:struct", "CHEMISTRY/HMDB2StructMapping.tsv", "Database input file, containing four tab-separated columns of identifier, name, SMILES, INCHI."
                                                                        "The identifier should match with mapping file. SMILES and INCHI are reported in the output, but not used otherwise. "
                                                                        "By default CHEMISTRY/HMDB2StructMapping.tsv in OpenMS/share is used! If empty, the default will be used.");
    defaults_.setValue("positive_adducts_file", "CHEMISTRY/PositiveAdducts.tsv", "This file contains the list of potential positive adducts that will be looked for in the database. "
                                                                                 "Edit the list if you wish to exclude/include adducts. "
                                                                                 "By default CHEMISTRY/PositiveAdducts.tsv in OpenMS/share is used! If empty, the default will be used.", ListUtils::create<String>("advanced"));
    defaults_.setValue("negative_adducts_file", "CHEMISTRY/NegativeAdducts.tsv", "This file contains the list of potential negative adducts that will be looked for in the database. "
                                                                                 "Edit the list if you wish to exclude/include adducts. "
                                                                                 "By default CHEMISTRY/NegativeAdducts.tsv in OpenMS/share is used! If empty, the default will be used.", ListUtils::create<String>("advanced"));


    defaultsToParam_();

    this->setLogType(CMD);
  }

  AccurateMassSearchEngine::~AccurateMassSearchEngine()
  {
  }

/// public methods

  void AccurateMassSearchEngine::queryByMass(const double& observed_mass, const Int& observed_charge, const String& ion_mode, std::vector<AccurateMassSearchResult>& results) const
  {
    if (!is_initialized_)
    {
      throw Exception::IllegalSelfOperation(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }

    // Depending on ion_mode_internal_, the file containing the rules for positive or negative adducts is loaded
    std::vector<AdductInfo_>::const_iterator it_s, it_e;
    if (ion_mode == "positive")
    {
      it_s = pos_adducts_.begin();
      it_e = pos_adducts_.end();
    }
    else if (ion_mode == "negative")
    {
      it_s = neg_adducts_.begin();
      it_e = neg_adducts_.end();
    }
    else
    {
      throw Exception::InvalidParameter(__FILE__, __LINE__, __PRETTY_FUNCTION__, String("Ion mode cannot be set to '") + ion_mode + "'!");
    }


    for (std::vector<AdductInfo_>::const_iterator it = it_s; it != it_e; ++it)
    {
      if (observed_charge != 0 && (std::abs(observed_charge) != std::abs(it->getCharge())))
      { // charge must match in absolute terms (absolute, since any FeatureFinder gives only positive charges, even for negative-mode spectra)
        // observed_charge==0 will pass, since we basically do not know its real charge (apparently, no isotopes were found)
        continue;
      }

      // get potential hits as indices in masskey_table
      double neutral_mass = it->getNeutralMass(observed_mass);
      std::pair<Size, Size> hit_idx;
      searchMass_(neutral_mass, hit_idx);

      //std::cerr << ion_mode_internal_ << " adduct: " << adduct_name << ", " << adduct_mass << " Da, " << query_mass << " qm(against DB), " << charge << " q\n";

      // store information from query hits in AccurateMassSearchResult objects
      for (Size i = hit_idx.first; i < hit_idx.second; ++i)
      {
        // check if DB entry is compatible to the adduct
        if (!it->isCompatible(EmpiricalFormula(mass_mappings_[i].formula)))
        {
          // only written if TOPP tool has --debug
          LOG_DEBUG << "'" << mass_mappings_[i].formula << "' cannot have adduct '" << it->getName() << "'. Omitting.\n";
          continue;
        }

        double found_mass(mass_mappings_[i].mass);
        double found_error_ppm(((neutral_mass - found_mass) / neutral_mass) * 1e6);

        AccurateMassSearchResult ams_result;
        ams_result.setAdductMass(observed_mass);
        ams_result.setQueryMass(neutral_mass);
        ams_result.setFoundMass(found_mass);
        ams_result.setCharge(observed_charge);
        ams_result.setErrorPPM(found_error_ppm);
        ams_result.setMatchingIndex(i);
        ams_result.setFoundAdduct(it->getName());
        ams_result.setEmpiricalFormula(mass_mappings_[i].formula);
        ams_result.setMatchingHMDBids(mass_mappings_[i].massIDs);

        results.push_back(ams_result);

        // ams_result.outputResults();
        // std::cout << "****************************************************" << std::endl;
      }

    }

    // if result is empty, add a 'not-found' indicator
    if (results.empty())
    {
      AccurateMassSearchResult ams_result;
      ams_result.setAdductMass(observed_mass);
      ams_result.setQueryMass(std::numeric_limits<double>::quiet_NaN());
      ams_result.setFoundMass(std::numeric_limits<double>::quiet_NaN());
      ams_result.setCharge(0); // cannot be NaN since Int, and -1 would be confusing too...
      ams_result.setErrorPPM(std::numeric_limits<double>::quiet_NaN());
      ams_result.setMatchingIndex(-1); // this is checked to identify 'not-found'
      ams_result.setFoundAdduct("-");
      ams_result.setEmpiricalFormula("");
      ams_result.setMatchingHMDBids(std::vector<String>(1, "--dummy--"));
      results.push_back(ams_result);
    }


    return;
  }

  void AccurateMassSearchEngine::queryByFeature(const Feature& feature, const Size& feature_index, const String& ion_mode, std::vector<AccurateMassSearchResult>& results) const
  {
    if (!is_initialized_)
    {
      throw Exception::IllegalSelfOperation(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }

    std::vector<AccurateMassSearchResult> results_part;

    queryByMass(feature.getMZ(), feature.getCharge(), ion_mode, results_part);

    for (Size hit_idx = 0; hit_idx < results_part.size(); ++hit_idx)
    {
      results_part[hit_idx].setObservedRT(feature.getRT());
      results_part[hit_idx].setSourceFeatureIndex(feature_index);
      results_part[hit_idx].setObservedIntensity(feature.getIntensity());
      // append
      results.push_back(results_part[hit_idx]);
    }
  }

  void AccurateMassSearchEngine::queryByConsensusFeature(const ConsensusFeature& cfeat, const Size& cf_index, const Size& number_of_maps, const String& ion_mode, std::vector<AccurateMassSearchResult>& results) const
  {
    if (!is_initialized_)
    {
      throw Exception::IllegalSelfOperation(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }

    std::vector<AccurateMassSearchResult> results_part;

    queryByMass(cfeat.getMZ(), cfeat.getCharge(), ion_mode, results_part);

    ConsensusFeature::HandleSetType ind_feats(cfeat.getFeatures());


    //    for ( ; f_it != ind_feats.end(); ++f_it)
    //    {
    //        std::cout << f_it->getRT() << "\t" << f_it->getMZ() << "\t" << f_it->getIntensity() << std::endl;
    //    }

    ConsensusFeature::const_iterator f_it = ind_feats.begin();
    std::vector<double> tmp_f_ints;
    for (Size map_idx = 0; map_idx < number_of_maps; ++map_idx)
    {
      // std::cout << "map idx: " << f_it->getMapIndex() << std::endl;
      if (map_idx == f_it->getMapIndex())
      {
        tmp_f_ints.push_back(f_it->getIntensity());
        ++f_it;
      }
      else
      {
        tmp_f_ints.push_back(0.0);
      }
    }


    for (Size hit_idx = 0; hit_idx < results_part.size(); ++hit_idx)
    {
      results_part[hit_idx].setObservedRT(cfeat.getRT());
      results_part[hit_idx].setSourceFeatureIndex(cf_index);
      // results_part[hit_idx].setObservedIntensity(cfeat.getIntensity());
      results_part[hit_idx].setIndividualIntensities(tmp_f_ints);
    }

    std::copy(results_part.begin(), results_part.end(), std::back_inserter(results));
  }

  void AccurateMassSearchEngine::init()
  {
    // Loads the default mapping file (chemical formulas -> HMDB IDs)
    parseMappingFile_(db_mapping_file_);
    // This loads additional properties like common name, smiles, and inchi key for each HMDB id
    parseStructMappingFile_(db_struct_file_);

    parseAdductsFile_(pos_adducts_fname_, pos_adducts_);
    parseAdductsFile_(neg_adducts_fname_, neg_adducts_);

    is_initialized_ = true;
  }

  void AccurateMassSearchEngine::run(FeatureMap<>& fmap, MzTab& mztab_out)  const
  {
    if (!is_initialized_)
    {
      throw Exception::IllegalSelfOperation(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }
    
    String ion_mode_internal(ion_mode_);
    if (ion_mode_ == "auto")
    {
      ion_mode_internal = resolveAutoMode_(fmap);
    }

    // map for storing overall results
    QueryResultsTable overall_results;
    Size dummy_count(0);
    for (Size i = 0; i < fmap.size(); ++i)
    {
      std::vector<AccurateMassSearchResult> query_results;

      // std::cout << i << ": " << fmap[i].getMetaValue(3) << " mass: " << fmap[i].getMZ() << " num_traces: " << fmap[i].getMetaValue("num_of_masstraces") << " charge: " << fmap[i].getCharge() << std::endl;
      queryByFeature(fmap[i], i, ion_mode_internal, query_results);

      if (query_results.size() == 0) continue; // cannot happen if a 'not-found' dummy was added

      bool is_dummy = (query_results[0].getMatchingIndex() == -1);
      if (is_dummy) ++dummy_count;

      if (iso_similarity_ && !is_dummy)
      {
        if (!fmap[i].metaValueExists("num_of_masstraces"))
        {
          LOG_WARN << "Feature does not contain meta value 'num_of_masstraces'. Cannot compute isotope similarity.";
        }
        else if ((Size)fmap[i].getMetaValue("num_of_masstraces") > 1)
        { // compute isotope pattern similarities (do not take the best-scoring one, since it might have really bad ppm or other properties -- 
          // it is impossible to decide here which one is best
          double best_iso_sim(std::numeric_limits<double>::max());
          Size best_iso_idx(0);
          for (Size hit_idx = 0; hit_idx < query_results.size(); ++hit_idx)
          {
            String emp_formula(query_results[hit_idx].getFormulaString());
            double iso_sim(computeIsotopePatternSimilarity_(fmap[i], EmpiricalFormula(emp_formula)));
            query_results[hit_idx].setIsotopesSimScore(iso_sim);
            /*
            if (iso_sim > best_iso_sim)
            {
              best_iso_sim = iso_sim;
              best_iso_idx = hit_idx;
            }
            */
          }
        
          //std::vector<AccurateMassSearchResult> tmp_results;
          //tmp_results.push_back(query_results[best_iso_idx]);

          // keep the best AccurateMassSearchResult, drop all other hits
          //query_results = tmp_results;
        }
      }

      // debug output
      //        for (Size hit_idx = 0; hit_idx < query_results.size(); ++hit_idx)
      //        {
      //            query_results[hit_idx].outputResults();
      //        }

      // String feat_label(fmap[i].getMetaValue(3));
      overall_results.push_back(query_results);
      annotate_(query_results, fmap[i]);
    }
    // add dummy protein identification which is required to keep peptidehits alive during store()
    fmap.getProteinIdentifications().resize(fmap.getProteinIdentifications().size() + 1);
    fmap.getProteinIdentifications().back().setIdentifier("AccurateMassSearchResult");
    fmap.getProteinIdentifications().back().setSearchEngine("AccurateMassSearchResult");
    fmap.getProteinIdentifications().back().setDateTime(DateTime().now());

    if (fmap.empty())
    {
      LOG_INFO << "FeatureMap was empty! No hits found!" << std::endl;
    }
    else
    { // division by 0 if used on empty fmap
      LOG_INFO << "\nFound " << (overall_results.size() - dummy_count) << " matched masses (with at least one hit each)\nfrom " << fmap.size() << " features\n  --> " << (overall_results.size()-dummy_count)*100/fmap.size() << "% explained" << std::endl;
    }
  
    exportMzTab_(overall_results, mztab_out);

    return;
  }

  void AccurateMassSearchEngine::annotate_(const std::vector<AccurateMassSearchResult>& amr, BaseFeature& f) const
  {
    f.getPeptideIdentifications().resize(f.getPeptideIdentifications().size() + 1);
    f.getPeptideIdentifications().back().setIdentifier("AccurateMassSearchResult");
    for (std::vector<AccurateMassSearchResult>::const_iterator it_row  = amr.begin();
                                                               it_row != amr.end();
                                                               ++it_row)
    {
      PeptideHit hit;
      hit.setMetaValue("identifier", it_row->getMatchingHMDBids());
      StringList names;
      for (Size i = 0; i < it_row->getMatchingHMDBids().size(); ++i)
      { // mapping ok?
        if (!hmdb_properties_mapping_.count(it_row->getMatchingHMDBids()[i]))
        {
          throw Exception::MissingInformation(__FILE__, __LINE__, __PRETTY_FUNCTION__, String("DB entry '") + it_row->getMatchingHMDBids()[i] + "' not found in struct file!");
        }
        // get name from index 0 (2nd column in structMapping file)
        HMDBPropsMapping::const_iterator entry = hmdb_properties_mapping_.find(it_row->getMatchingHMDBids()[i]);
        if  (entry == hmdb_properties_mapping_.end())
        {
          throw Exception::MissingInformation(__FILE__, __LINE__, __PRETTY_FUNCTION__, String("DB entry '") + it_row->getMatchingHMDBids()[i] + "' found in struct file but missing in mapping file!");
        }
        names.push_back(entry->second[0]);
      }
      hit.setMetaValue("description", names);
      hit.setMetaValue("charge", it_row->getCharge());
      hit.setMetaValue("modifications", it_row->getFoundAdduct());
      hit.setMetaValue("chemical_formula", it_row->getFormulaString());
      hit.setMetaValue("ppm_error", it_row->getErrorPPM());
      f.getPeptideIdentifications().back().insertHit(hit);
    }
  }

  void AccurateMassSearchEngine::run(ConsensusMap& cmap, MzTab& mztab_out)  const
  {
    if (!is_initialized_)
    {
      throw Exception::IllegalSelfOperation(__FILE__, __LINE__, __PRETTY_FUNCTION__);
    }

    String ion_mode_internal(ion_mode_);
    if (ion_mode_ == "auto")
    {
      ion_mode_internal = resolveAutoMode_(cmap);
    }

    ConsensusMap::FileDescriptions fd_map = cmap.getFileDescriptions();
    Size num_of_maps = fd_map.size();

    // map for storing overall results
    QueryResultsTable overall_results;

    for (Size i = 0; i < cmap.size(); ++i)
    {
      std::vector<AccurateMassSearchResult> query_results;
      // std::cout << i << ": " << cmap[i].getMetaValue(3) << " mass: " << cmap[i].getMZ() << " num_traces: " << cmap[i].getMetaValue("num_of_masstraces") << " charge: " << cmap[i].getCharge() << std::endl;
      queryByConsensusFeature(cmap[i], i, num_of_maps, ion_mode_internal, query_results);
      annotate_(query_results, cmap[i]);
      overall_results.push_back(query_results);
    }
    // add dummy protein identification which is required to keep peptidehits alive during store()
    cmap.getProteinIdentifications().resize(cmap.getProteinIdentifications().size() + 1);
    cmap.getProteinIdentifications().back().setIdentifier("AccurateMassSearchResult");
    cmap.getProteinIdentifications().back().setSearchEngine("AccurateMassSearchResult");
    cmap.getProteinIdentifications().back().setDateTime(DateTime().now());

    exportMzTab_(overall_results, mztab_out);
    return;
  }

  void AccurateMassSearchEngine::exportMzTab_(const QueryResultsTable& overall_results, MzTab& mztab_out) const
  {
    // iterate the overall results table

    String unit_id("AccMassSearch");
    MzTabSmallMoleculeSectionData sm_data_section;
    MzTabSmallMoleculeSectionRows all_sm_rows;

    Size id_group(1);

    std::map<String, UInt> adduct_stats; // adduct --> # occurences
    std::map<String, std::set<Size> > adduct_stats_unique; // adduct --> # occurences (count each feature only once)

    for (QueryResultsTable::const_iterator tab_it = overall_results.begin(); tab_it != overall_results.end(); ++tab_it)
    {
      // std::cout << tab_it->first << std::endl;

      for (Size hit_idx = 0; hit_idx < tab_it->size(); ++hit_idx)
      {
        // tab_it->second[hit_idx].outputResults();

        std::vector<String> matching_ids = (*tab_it)[hit_idx].getMatchingHMDBids();

        // iterate over multiple IDs, generate a new row for each one

        for (Size id_idx = 0; id_idx < matching_ids.size(); ++id_idx)
        {
          MzTabSmallMoleculeSectionRow mztab_row_record;

          // set the identifier field
          String hid_temp = matching_ids[id_idx];
          MzTabString hmdb_id;
          hmdb_id.set(hid_temp);
          std::vector<MzTabString> hmdb_id_dummy;
          hmdb_id_dummy.push_back(hmdb_id);
          MzTabStringList string_dummy_list;
          string_dummy_list.set(hmdb_id_dummy);

          mztab_row_record.identifier = string_dummy_list;

          // set the chemical formula field
          MzTabString chem_form;
          String form_temp = (*tab_it)[hit_idx].getFormulaString();
          chem_form.set(form_temp);

          mztab_row_record.chemical_formula = chem_form;

          HMDBPropsMapping::const_iterator entry = hmdb_properties_mapping_.find(hid_temp);

          // set the smiles field
          String smi_temp = entry->second[1];             // extract SMILES from struct mapping file
          MzTabString smi_string;
          smi_string.set(smi_temp);

          mztab_row_record.smiles = smi_string;

          // set the inchi_key field
          String inchi_temp = entry->second[2];             // extract INCHIKEY from struct mapping file
          MzTabString inchi_key;
          inchi_key.set(inchi_temp);

          mztab_row_record.inchi_key = inchi_key;

          // set description field (we use it for the common name of the compound)
          String name_temp = entry->second[0];
          MzTabString common_name;
          common_name.set(name_temp);

          mztab_row_record.description = common_name;


          // set mass_to_charge field
          double mz_temp = (*tab_it)[hit_idx].getAdductMass();
          MzTabDouble mass_to_charge;
          mass_to_charge.set(mz_temp);

          mztab_row_record.mass_to_charge = mass_to_charge;


          // set charge field
          Int ch_temp = (*tab_it)[hit_idx].getCharge();
          MzTabDouble mcharge;
          mcharge.set(ch_temp);

          mztab_row_record.charge = mcharge;


          // set RT field
          MzTabDouble rt_temp;
          rt_temp.set((*tab_it)[hit_idx].getObservedRT());
          std::vector<MzTabDouble> rt_temp3(1, rt_temp);
          MzTabDoubleList observed_rt;
          observed_rt.set(rt_temp3);

          mztab_row_record.retention_time = observed_rt;


          // set database field
          String dbname_temp = "HMDB";
          MzTabString dbname;
          dbname.set(dbname_temp);

          mztab_row_record.database = dbname;


          // set database_version field
          String dbver_temp = "3.5";
          MzTabString dbversion;
          dbversion.set(dbver_temp);

          mztab_row_record.database_version = dbversion;


          // set smallmolecule_abundance_sub
          // check if we deal with a feature or consensus feature

          std::vector<double> indiv_ints(tab_it->at(hit_idx).getIndividualIntensities());
          std::vector<MzTabDouble> int_temp3;

          if (indiv_ints.size() == 0)
          {
            double int_temp((*tab_it)[hit_idx].getObservedIntensity());
            MzTabDouble int_temp2;
            int_temp2.set(int_temp);
            int_temp3.push_back(int_temp2);
          }
          else
          {
            for (Size ii = 0; ii < indiv_ints.size(); ++ii)
            {
              double int_temp(indiv_ints[ii]);
              MzTabDouble int_temp2;
              int_temp2.set(int_temp);
              int_temp3.push_back(int_temp2);
            }
          }

          mztab_row_record.smallmolecule_abundance_sub = int_temp3;


          // set smallmolecule_abundance_stdev_sub; not applicable for a single feature intensity, however must be filled. Otherwise, the mzTab export fails.
          MzTabDouble stdev_temp;
          stdev_temp.set(0.0);
          std::vector<MzTabDouble> stdev_temp3;

          if (indiv_ints.size() == 0)
          {
            stdev_temp3.push_back(stdev_temp);
          }
          else
          {
            for (Size ii = 0; ii < indiv_ints.size(); ++ii)
            {
              stdev_temp3.push_back(stdev_temp);
            }
          }

          mztab_row_record.smallmolecule_abundance_stdev_sub = stdev_temp3;


          // set smallmolecule_abundance_std_error_sub; not applicable for a single feature intensity, however must be filled. Otherwise, the mzTab export fails.
          MzTabDouble stderr_temp2;
          stderr_temp2.set(0.0);
          std::vector<MzTabDouble> stderr_temp3;

          if (indiv_ints.size() == 0)
          {
            stderr_temp3.push_back(stderr_temp2);
          }
          else
          {
            for (Size ii = 0; ii < indiv_ints.size(); ++ii)
            {
              stderr_temp3.push_back(stderr_temp2);
            }
          }


          mztab_row_record.smallmolecule_abundance_std_error_sub = stderr_temp3;


          // optional columns:
          std::vector<MzTabOptionalColumnEntry> optionals;

          // ppm error
          MzTabString ppmerr;
          ppmerr.set(String((*tab_it)[hit_idx].getErrorPPM()));
          MzTabOptionalColumnEntry col0;
          col0.first = "opt_ppm_error";
          col0.second = ppmerr;
          optionals.push_back(col0);

          // set found adduct ion
          String addion_temp((*tab_it)[hit_idx].getFoundAdduct());
          MzTabString addion;
          addion.set(addion_temp);
          MzTabOptionalColumnEntry col1;
          col1.first = "opt_adduct_ion";
          col1.second = addion;
          optionals.push_back(col1);
          ++adduct_stats[addion_temp];       // just some stats


          // set isotope similarity score
          double sim_score_temp((*tab_it)[hit_idx].getIsotopesSimScore());
          std::stringstream read_in;
          read_in << sim_score_temp;
          String sim_score_temp2(read_in.str());
          MzTabString sim_score;
          sim_score.set(sim_score_temp2);
          MzTabOptionalColumnEntry col2;
          col2.first = "opt_isosim_score";
          col2.second = sim_score;
          optionals.push_back(col2);


          // set id group; rows with the same id group number originated from the same feature
          adduct_stats_unique[addion_temp].insert(id_group);       // stats ...
          String id_group_temp(id_group);
          MzTabString id_group_str;
          id_group_str.set(id_group_temp);
          MzTabOptionalColumnEntry col3;
          col3.first = "opt_id_group";
          col3.second = id_group_str;
          optionals.push_back(col3);
          mztab_row_record.opt_ = optionals;
          all_sm_rows.push_back(mztab_row_record);
        }
      }
      ++id_group;
    }

    sm_data_section[unit_id] = all_sm_rows;
    mztab_out.setSmallMoleculeSectionData(sm_data_section);


    // print some adduct stats:
    LOG_INFO << "Hits by adduct: #peaks explained (# matching db entries)'\n";
    for (std::map<String, UInt>::const_iterator it = adduct_stats.begin(); it != adduct_stats.end(); ++it)
    {
      LOG_INFO << "  '" << it->first << "' : " << adduct_stats_unique[it->first].size() << " (" << it->second << ")\n";
    }
    LOG_INFO << std::endl;

  }

/// protected methods

  void AccurateMassSearchEngine::updateMembers_()
  {
    mass_error_value_ = (double)param_.getValue("mass_error_value");
    mass_error_unit_ = (String)param_.getValue("mass_error_unit");
    ion_mode_ = (String)param_.getValue("ionization_mode");

    iso_similarity_ = param_.getValue("isotopic_similarity").toBool();

    // use defaults if empty for all .tsv files
    db_mapping_file_ = (String)param_.getValue("db:mapping");
    if (db_mapping_file_.trim().empty()) db_mapping_file_ = (String)defaults_.getValue("db:mapping");
    db_struct_file_ = (String)param_.getValue("db:struct");
    if (db_struct_file_.trim().empty()) db_struct_file_ = (String)defaults_.getValue("db:struct");

    pos_adducts_fname_ = (String)param_.getValue("positive_adducts_file");
    if (pos_adducts_fname_.trim().empty()) pos_adducts_fname_ = (String)defaults_.getValue("positive_adducts_file");
    neg_adducts_fname_ = (String)param_.getValue("negative_adducts_file");
    if (neg_adducts_fname_.trim().empty()) neg_adducts_fname_ = (String)defaults_.getValue("negative_adducts_file");

    // database names might have changed, so parse files again before next query
    is_initialized_ = false;
  }

/// private methods

  void AccurateMassSearchEngine::parseMappingFile_(const String& db_mapping_file)
  {
    mass_mappings_.clear();

    // load map_fname mapping file
    String filename = db_mapping_file;

    // load map_fname mapping file
    if (!File::readable(filename))
    {
      // throws Exception::FileNotFound if not found
      filename = File::find(filename);
    }

    String line;
    Size line_count(0);
    std::stringstream str_buf;
    std::istream_iterator<String> eol;

    // LOG_DEBUG << "parsing " << fname << " file..." << std::endl;

    std::ifstream ifs(filename.c_str());
    while (getline(ifs, line))
    {
      ++line_count;
      line.trim();
      if (line.empty()) continue;

      str_buf.clear();
      str_buf << line;
      // std::cout << line << std::endl;
      std::istream_iterator<String> istr_it(str_buf);

      Size word_count(0);
      MappingEntry_ entry;

      while (istr_it != eol)
      {
        // LOG_DEBUG << *istr_it << " ";
        if (word_count == 0)
        {
          entry.mass = istr_it->toDouble();
        }
        else if (word_count == 1)
        {
          entry.formula = *istr_it;
          if (entry.mass == 0)
          { // recompute mass from formula
            entry.mass = EmpiricalFormula(entry.formula).getMonoWeight();
            //std::cerr << "mass of " << entry.formula << " is " << entry.mass << "\n";
          }
        }
        else // one or more IDs can follow
        {
          entry.massIDs.push_back(*istr_it);
        }

        ++word_count;
        ++istr_it;
      }
      // LOG_DEBUG << std::endl;

      if (entry.massIDs.empty())
      {
        throw Exception::InvalidParameter(__FILE__, __LINE__, __PRETTY_FUNCTION__, String("File '") + db_mapping_file + "' in line " + line_count + " as '" + line + "' cannot be parsed. Found " + word_count + " entries, expected at least three!");
      }
      mass_mappings_.push_back(entry);
    }

    std::sort(mass_mappings_.begin(), mass_mappings_.end(), CompareEntryAndMass_());

    LOG_INFO << "Read " << mass_mappings_.size() << " entries from mapping file!" << std::endl;

    return;
  }

  void AccurateMassSearchEngine::parseStructMappingFile_(const String& db_struct_file)
  {
    hmdb_properties_mapping_.clear();

    String filename = db_struct_file;

    // load map_fname mapping file
    if (!File::readable(filename))
    {
      // throws Exception::FileNotFound if not found
      filename = File::find(filename);
    }

    std::ifstream ifs(filename.c_str());
    String line;
    // LOG_DEBUG << "parsing " << fname << " file..." << std::endl;

    std::vector<String> parts;
    while (getline(ifs, line))
    {
      line.trim();
      line.split("\t", parts);

      if (parts.size() == 4)
      {
        String hmdb_id_key(parts[0]);

        if (hmdb_properties_mapping_.count(hmdb_id_key))
        {
          throw Exception::InvalidParameter(__FILE__, __LINE__, __PRETTY_FUNCTION__, String("File '") + db_struct_file + "' in line '" + line + "' cannot be parsed. The HMDB ID entry was already used (see above)!");
        }
        std::copy(parts.begin() + 1, parts.end(), std::back_inserter(hmdb_properties_mapping_[hmdb_id_key]));
      }
      else
      {
        throw Exception::InvalidParameter(__FILE__, __LINE__, __PRETTY_FUNCTION__, String("File '") + db_struct_file + "' in line '" + line + "' cannot be parsed. Expected four entries separated by tab. Found " + parts.size() + " entries!");
      }

    }

    // add our dummy, so mzTab annotation does not discard 'not-found' features
    std::vector<String> dummy_data(3, "-");
    dummy_data[0] = "--dummy--";
    hmdb_properties_mapping_["--dummy--"] = dummy_data;

    return;
  }

  void AccurateMassSearchEngine::parseAdductsFile_(const String& filename, std::vector<AdductInfo_>& result)
  {
    result.clear();

    String fname = filename;
    // search for mapping file
    if (!File::readable(fname))
    { // throws Exception::FileNotFound if not found
      fname = File::find(filename);
    }
    TextFile tf(fname, true, -1, true); // trim & skip_empty
    for (TextFile::const_iterator it = tf.begin(); it != tf.end(); ++it)
    {
      result.push_back(AdductInfo_::parseAdductString(*it));
    }

    LOG_INFO << "Read " << result.size() << " entries from adduct file '" << fname << "'." << std::endl;

    return;
  }

  void AccurateMassSearchEngine::searchMass_(const double& neutral_query_mass, std::pair<Size, Size>& hit_indices) const
  {
    double diff_mz;
    // check if mass error window is given in ppm or Da
    if (mass_error_unit_ == "ppm")
    {
      diff_mz = (neutral_query_mass / 1e6) * mass_error_value_;
    }
    else
    {
      diff_mz = mass_error_value_;
    }

    //LOG_INFO << "searchMass: neutral_query_mass=" << neutral_query_mass << " diff_mz=" << diff_mz << " ppm allowed:" << mass_error_value_ << std::endl;

    // binary search for formulas which are within diff_mz distance
    if (mass_mappings_.empty())
    {
      throw Exception::InvalidValue(__FILE__, __LINE__, __PRETTY_FUNCTION__, "There are no entries found in mass-to-ids mapping file! Aborting... ", "0");
    }

    std::vector<MappingEntry_>::const_iterator lower_it = std::lower_bound(mass_mappings_.begin(), mass_mappings_.end(), neutral_query_mass - diff_mz, CompareEntryAndMass_()); // first element equal or larger
    std::vector<MappingEntry_>::const_iterator upper_it = std::upper_bound(mass_mappings_.begin(), mass_mappings_.end(), neutral_query_mass + diff_mz, CompareEntryAndMass_()); // first element greater than

    //std::cout << *lower_it << " " << *upper_it << "idx: " << lower_it - masskey_table_.begin() << " " << upper_it - masskey_table_.begin() << std::endl;
    Size start_idx = std::distance(mass_mappings_.begin(), lower_it);
    Size end_idx = std::distance(mass_mappings_.begin(), upper_it);

    //hit_indices.clear();
    //hit_indices.reserve(end_idx - start_idx);
    hit_indices.first = start_idx;
    hit_indices.second = end_idx;

    //for (Size hit_idx = start_idx; hit_idx < end_idx; ++hit_idx)
    {
      //hit_indices.push_back(hit_idx);
      //double found_mass(mass_mappings_[hit_idx].mass);
      //double found_error_ppm(((neutral_query_mass - found_mass)/neutral_query_mass)*1e6);
      // debug output
      //std::cout << std::setprecision(10) << "found mass: " << found_mass  << " with error: " << found_error_ppm << std::endl;
    }

    return;
  }

  double AccurateMassSearchEngine::computeCosineSim_( const std::vector<double>& x, const std::vector<double>& y ) const
  {
    if (x.size() != y.size())
    {
      return 0.0;
    }

    double mixed_sum(0.0);
    double x_squared_sum(0.0);
    double y_squared_sum(0.0);


    for (Size i = 0; i < x.size(); ++i)
    {
      mixed_sum += x[i] * y[i];
      x_squared_sum += x[i] * x[i];
      y_squared_sum += y[i] * y[i];
    }

    double denom(std::sqrt(x_squared_sum) * std::sqrt(y_squared_sum));

    return (denom > 0.0) ? mixed_sum / denom : 0.0;
  }

  double AccurateMassSearchEngine::computeEuclideanDist_( const std::vector<double>& x, const std::vector<double>& y ) const
  {
    if (x.size() != y.size())
    {
      return -100.0;
    }

    double sum_of_squares(0.0);

    for (Size i = 0; i < x.size(); ++i)
    {
      sum_of_squares += (x[i] - y[i]) * (x[i] - y[i]);
    }

    return std::sqrt(sum_of_squares);
  }

  double AccurateMassSearchEngine::computeIsotopePatternSimilarity_(const Feature& feat, const EmpiricalFormula& form) const
  {
    Size num_traces = (Size)feat.getMetaValue("num_of_masstraces");
    const Size MAX_THEORET_ISOS(5);

    Size common_size = std::min(num_traces, MAX_THEORET_ISOS);

    IsotopeDistribution iso_dist(form.getIsotopeDistribution(common_size));

    double max_iso_prob(iso_dist.begin()->second);

    // scale available peaks to 1
    iso_dist.renormalize();
    std::vector<double> normed_iso_ratios;
    // std::cout << "theoret. iso: ";
    for (IsotopeDistribution::ConstIterator iso_it = iso_dist.begin(); iso_it != (iso_dist.begin() + common_size); ++iso_it)
    {
      normed_iso_ratios.push_back(iso_it->second);
      // std::cout << temp_ratio << " ";
    }

    // std::cout << "\nact. iso: ";
    
    // same for observed isotope distribution
    double max_feat_int((double)feat.getMetaValue("masstrace_intensity_0"));
    std::vector<double> normed_feat_ratios;
    for (Size int_idx = 0; int_idx < common_size; ++int_idx)
    {
      double mt_int = (double)feat.getMetaValue("masstrace_intensity_" + String(int_idx));
      normed_feat_ratios.push_back(mt_int);

      if (mt_int > max_feat_int)
      {
        max_feat_int = mt_int;
      }
    }

    // normalize with max isotope intensity
    for (Size int_idx = 0; int_idx < common_size; ++int_idx)
    {
      normed_feat_ratios[int_idx] /= max_feat_int;
    }

    return computeCosineSim_(normed_iso_ratios, normed_feat_ratios);
    // return computeEuclideanDist_(normed_iso_ratios, normed_feat_ratios);
  }

} // closing namespace OpenMS
