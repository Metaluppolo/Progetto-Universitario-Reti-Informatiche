# 1. COMPILAZIONE
# Il comando 'make' necessita del makefile, che deve essere
# creato come descritto nella guida sulla pagina Elearn

  make

  read -p "Compilazione eseguita. Premi invio per eseguire..."

# 2. ESECUZIONE
# I file eseguibili di discovery server e peer devono
# chiamarsi 'ds' e 'peer', e devono essere nella current folder

# 2.1 esecuzioe del DS sulla porta 4242
  gnome-terminal -x sh -c "./ds 4242; exec bash"

# 2.2 esecuzione di 5 peer sulle porte {2421,...,2425}
  for port in {2421..2425}
  do
     gnome-terminal -x sh -c "./peer $port 4242; exec bash"
  done
