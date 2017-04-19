// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2016.
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
// $Maintainer: Lukas Zimmermann $
// $Authors: Lukas Zimmermann $
// --------------------------------------------------------------------------
#include <OpenMS/FORMAT/XQuestResultXMLFile.h>
#include <OpenMS/FORMAT/HANDLERS/XQuestResultXMLHandler.h>
#include <OpenMS/METADATA/PeptideIdentification.h>

namespace OpenMS
{
  XQuestResultXMLFile::XQuestResultXMLFile() :
    XMLFile("/SCHEMAS/xQuest_1_0.xsd", "1.0"),
    n_hits_(-1)
  {
  }
  XQuestResultXMLFile::~XQuestResultXMLFile()
  {
  }

  void XQuestResultXMLFile::load(const String & filename,
                                 std::vector< std::vector < PeptideIdentification > > & csms,
                                 std::vector< ProteinIdentification > & prot_ids,
                                 Size min_n_ions_per_spectrum,
                                 bool load_to_peptideHit)
  {
   Internal::XQuestResultXMLHandler handler(filename, csms, prot_ids, this->n_hits_, this->min_score_, this->max_score_,
                                            min_n_ions_per_spectrum, load_to_peptideHit);
   this->parse_(filename, &handler);

   // TODO Whishful thinking: Get the attributes above from the handler once parsing has been finished
   this->n_hits_ = handler.getNumberOfHits();
   this->min_score_ = handler.getMinScore();
   this->max_score_ = handler.getMaxScore();
  }


  /* TODO Currently not implemented
  void XQuestResultXMLFile::store(const String & filename, std::vector<std::vector<PeptideIdentification> > & spectra)
  {
    // TODO Currently dummy, needed for the handler
    std::vector< XQuestResultMeta > metas;
    std::vector< ProteinIdentification > prot_ids;
    
    this->n_hits_= 0;
    Internal::XQuestResultXMLHandler handler(filename, metas,spectra, prot_ids, this->n_hits_, NULL, 0, false);
  }
  */



  int XQuestResultXMLFile::getNumberOfHits() const
  {
    return this->n_hits_;
  }

  double XQuestResultXMLFile::getMinScore() const
  {
    return this->min_score_;
  }

  double XQuestResultXMLFile::getMaxScore() const
  {
    return this->max_score_;
  }
}
